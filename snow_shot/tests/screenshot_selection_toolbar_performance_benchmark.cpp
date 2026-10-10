#include "snow_shot/presentation/screenshotselectiontoolbarwidget.h"
#include "snow_shot/presentation/screenshottoolbarcommands.h"
#ifndef SNOW_SELECTION_TOOLBAR_BASELINE
#include "snow_shot/presentation/screenshotselectionlimits.h"
#include "snow_shot/presentation/screenshotselectionmodel.h"
#include "widgets/select.h"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QWidget>

#include <algorithm>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {
class ToolbarCommands final : public ScreenshotSelectionToolbarCommandSink {
  public:
    void toggleSelectionAspectRatioLockFromToolbar() override {}
    void setSelectionAspectRatioPresetFromToolbar(ScreenshotSelectionAspectRatioPreset) override {}
    void openSelectionResizeModalFromToolbar() override {}
    void hideColorPickersForScreenshotUi() override {}
    void adjustSelectionFromToolbar(int, int, int, int) override {}
    void setSelectionCornerRadiusFromToolbar(int) override {}
    void setSelectionShadowWidthFromToolbar(int) override {}
    void setSelectionToolbarHovered(bool) override {}
};

template <typename Operation>
void measure(const char* name, int samples, int iterations, Operation operation) {
    for (int index = 0; index < iterations; ++index) {
        operation(index);
    }
    QCoreApplication::processEvents();
    std::vector<double> timings;
    timings.reserve(static_cast<std::size_t>(samples));
    for (int sample = 0; sample < samples; ++sample) {
        QElapsedTimer timer;
        timer.start();
        for (int index = 0; index < iterations; ++index) {
            operation(index);
        }
        timings.push_back(static_cast<double>(timer.nsecsElapsed()) / iterations / 1000.0);
        QCoreApplication::processEvents();
    }
    std::sort(timings.begin(), timings.end());
    const auto percentile = [&timings](double quantile) {
        const auto index =
            static_cast<std::size_t>(quantile * static_cast<double>(timings.size() - 1));
        return timings[index];
    };
    std::cout << name << ',' << std::fixed << std::setprecision(3) << percentile(0.5) << ','
              << percentile(0.95) << ',' << iterations << '\n';
}
} // namespace

int main(int argc, char* argv[]) {
    QApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("samples"), QStringLiteral("Sample batches"),
                      QStringLiteral("count"), QStringLiteral("30")});
    parser.addOption({QStringLiteral("iterations"), QStringLiteral("Geometry updates per batch"),
                      QStringLiteral("count"), QStringLiteral("2000")});
    parser.process(application);
    bool samplesValid = false;
    bool iterationsValid = false;
    const int samples = parser.value(QStringLiteral("samples")).toInt(&samplesValid);
    const int iterations = parser.value(QStringLiteral("iterations")).toInt(&iterationsValid);
    if (!samplesValid || !iterationsValid || samples < 3 || iterations < 10) {
        std::cerr << "samples must be at least 3 and iterations at least 10\n";
        return EXIT_FAILURE;
    }

    ToolbarCommands commands;
    QWidget host;
    host.resize(900, 500);
    ScreenshotSelectionToolbarWidget toolbar(commands, &host);
    const QRect selection(120, 140, 320, 180);
    const auto synchronize = [&toolbar](const QRect& rectangle) {
        toolbar.setPointerInteractionEnabled(true);
        toolbar.setSelectionResizable(true);
        toolbar.setCornerRadiusApplicable(true);
        toolbar.setSelectionState(rectangle, false, 8, 4,
                                  ScreenshotSelectionToolbarWidget::DisplayMode::Full, false,
                                  std::nullopt);
    };
    synchronize(selection);
    toolbar.move(100, 50);
    host.show();
    toolbar.show();
    QCoreApplication::processEvents();
    std::cout << "scenario,p50_batch_mean_us,p95_batch_mean_us,iterations\n";
    measure("same_state", samples, iterations * 10,
            [&synchronize, &selection](int) { synchronize(selection); });
    measure("dimensions", samples, iterations, [&synchronize, &selection](int index) {
        synchronize(QRect(selection.topLeft(),
                          QSize(selection.width() + index % 2, selection.height() + index % 2)));
    });
    const auto synchronizeSmart = [&toolbar](const QRect& rectangle) {
        toolbar.setPointerInteractionEnabled(true);
        toolbar.setSelectionResizable(false);
        toolbar.setCornerRadiusApplicable(true);
        toolbar.setSelectionState(rectangle, false, 8, 4,
                                  ScreenshotSelectionToolbarWidget::DisplayMode::SizeOnly, false,
                                  std::nullopt);
    };
    synchronizeSmart(selection);
    QCoreApplication::processEvents();
    measure("smart_position_only", samples, iterations, [&synchronizeSmart, &selection](int index) {
        synchronizeSmart(selection.translated(10000 + index % 2, -10000 - index % 2));
    });
    measure("smart_dimensions", samples, iterations, [&synchronizeSmart, &selection](int index) {
        synchronizeSmart(QRect(selection.topLeft(), QSize(selection.width() + index % 2,
                                                          selection.height() + index % 2)));
    });
    measure("smart_to_editing", samples, std::max(10, iterations / 10),
            [&synchronize, &synchronizeSmart, &selection](int index) {
                const QRect moved = selection.translated(index % 2, index % 2);
                synchronizeSmart(moved);
                synchronize(moved);
            });
#ifndef SNOW_SELECTION_TOOLBAR_BASELINE
    ScreenshotSelectionModel model;
    model.setSelectionRect(QRectF(selection));
    const QRectF bounds(0, 0, 1920, 1080);
    measure("ratio_selection", samples, iterations, [&toolbar, &model, &bounds](int index) {
        const auto preset = index % 2 == 0 ? ScreenshotSelectionAspectRatioPreset::Landscape16x9
                                           : ScreenshotSelectionAspectRatioPreset::Portrait9x16;
        static_cast<void>(model.setAspectRatioPreset(
            preset, bounds, snow_shot::presentation::kScreenshotSelectionMinimumSize));
        toolbar.setSelectionState(model.pixelSelection(), model.aspectRatioLocked(), 8, 4,
                                  ScreenshotSelectionToolbarWidget::DisplayMode::Full, false,
                                  std::nullopt, model.aspectRatioPreset());
    });
    auto* select = toolbar.findChild<adqt::widgets::AdSelect*>(
        QStringLiteral("screenshotSelectionAspectRatioSelect"));
    if (select == nullptr) {
        return EXIT_FAILURE;
    }
    measure("popup_open_close", samples, std::max(10, iterations / 100), [select](int) {
        select->showPopup();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        select->hidePopup();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    });
#endif
    return EXIT_SUCCESS;
}
