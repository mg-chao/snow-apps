#include "snow_shot/presentation/screenshotlatexrenderer.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QPainter>
#include <QTimer>

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

#if defined(Q_OS_WIN)
#include <Windows.h>
#include <Psapi.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <sys/resource.h>
#else
#include <fstream>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {

struct MemoryReading final {
    quint64 resident = 0;
    quint64 peakResident = 0;
    quint64 privateBytes = 0;
};

MemoryReading memoryReading() {
    MemoryReading reading;
#if defined(Q_OS_WIN)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                static_cast<DWORD>(sizeof(counters)))) {
        reading.resident = static_cast<quint64>(counters.WorkingSetSize);
        reading.peakResident = static_cast<quint64>(counters.PeakWorkingSetSize);
        reading.privateBytes = static_cast<quint64>(counters.PrivateUsage);
    }
#elif defined(Q_OS_MACOS)
    mach_task_basic_info_data_t counters{};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&counters),
                  &count) == KERN_SUCCESS) {
        reading.resident = static_cast<quint64>(counters.resident_size);
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        reading.peakResident = static_cast<quint64>(usage.ru_maxrss);
    }
#else
    quint64 virtualPages = 0;
    quint64 residentPages = 0;
    std::ifstream status("/proc/self/statm");
    const auto pageSize = sysconf(_SC_PAGESIZE);
    if (status >> virtualPages >> residentPages && pageSize > 0) {
        reading.resident = residentPages * static_cast<quint64>(pageSize);
    }
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
        reading.peakResident = static_cast<quint64>(usage.ru_maxrss) * 1024;
    }
#endif
    return reading;
}

void reportMemory(const char* phase) {
    const auto memory = memoryReading();
    std::cout << phase << " resident_bytes=" << memory.resident
              << " peak_resident_bytes=" << memory.peakResident
              << " private_bytes=" << memory.privateBytes << '\n';
}

void reportLatency(const char* phase, std::vector<qint64> durations) {
    std::sort(durations.begin(), durations.end());
    std::cout << phase << " samples=" << durations.size()
              << " median_ms=" << static_cast<double>(durations[durations.size() / 2]) / 1e6
              << " p95_ms=" << static_cast<double>(durations[durations.size() * 95 / 100]) / 1e6
              << '\n';
}

class Runner final {
  public:
    Runner() {
        QObject::connect(
            &m_renderer, &ScreenshotLatexRenderer::rendered, &m_renderer,
            [&](quint64 token, const QImage& image, ScreenshotLatexRenderer::Error error) {
                if (token != m_expected) {
                    ++m_stale;
                    return;
                }
                m_failed = error != ScreenshotLatexRenderer::Error::None;
                m_image = image;
                m_complete = true;
                ++m_completions;
            });
    }

    void enqueue(const QString& source, QSize viewport = QSize(480, 360)) {
        m_complete = false;
        m_failed = false;
        m_expected = m_renderer.request(source, viewport, 2, Qt::black);
        ++m_requests;
    }

    void wait() const {
        if (!m_complete) {
            QEventLoop loop;
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
            QObject::connect(&m_renderer, &ScreenshotLatexRenderer::rendered, &loop,
                             [&](quint64, const QImage&, ScreenshotLatexRenderer::Error) {
                                 if (m_complete)
                                     loop.quit();
                             });
            deadline.start(10000);
            loop.exec();
        }
        if (!m_complete || m_failed) {
            throw std::runtime_error("Formula benchmark render failed");
        }
    }

    qint64 render(const QString& source, QSize viewport = QSize(480, 360)) {
        QElapsedTimer elapsed;
        elapsed.start();
        enqueue(source, viewport);
        wait();
        return elapsed.nsecsElapsed();
    }

    void clear() {
        m_renderer.clearCache();
        static_cast<void>(render(QString())); // Worker barrier, also drops the displayed raster.
    }

    [[nodiscard]] const QImage& image() const {
        return m_image;
    }
    [[nodiscard]] int requests() const {
        return m_requests;
    }
    [[nodiscard]] int completions() const {
        return m_completions;
    }
    [[nodiscard]] int stale() const {
        return m_stale;
    }

  private:
    ScreenshotLatexRenderer m_renderer;
    QImage m_image;
    quint64 m_expected = 0;
    bool m_complete = false;
    bool m_failed = false;
    int m_requests = 0;
    int m_completions = 0;
    int m_stale = 0;
};

} // namespace

int main(int argc, char** argv) {
    QGuiApplication application(argc, argv);
    try {
        QString previewOutput;
        const auto arguments = application.arguments();
        const auto previewOption = arguments.indexOf(QStringLiteral("--preview-output"));
        if (previewOption >= 0) {
            if (previewOption + 1 >= arguments.size()) {
                throw std::runtime_error("--preview-output requires a PNG path");
            }
            previewOutput = arguments[previewOption + 1];
        }
        Runner runner;
        const std::array<QString, 3> formulas{
            QStringLiteral("\\frac{a+b}{\\sqrt{x^2+1}}"),
            QStringLiteral("\\sum_{n=0}^{\\infty}\\frac{x^n}{n!}"),
            QStringLiteral("\\begin{pmatrix}1&2&3\\\\4&5&6\\\\7&8&9\\end{pmatrix}")};
        reportMemory("before_initialization");
        const auto cold = runner.render(formulas[0]);
        const auto previewImage = runner.image();
        std::cout << "cold_ms=" << static_cast<double>(cold) / 1e6 << '\n';
        reportMemory("after_initialization");

        std::vector<qint64> durations;
        for (int i = 0; i < 60; ++i) {
            // Distinct inputs measure parsing/layout rather than the raster cache.
            durations.push_back(
                runner.render(formulas[static_cast<std::size_t>(i) % formulas.size()] +
                              QLatin1Char('+') + QString::number(i)));
        }
        reportLatency("warm_uncached", durations);
        reportMemory("after_warm_uncached");
        durations.clear();
        static_cast<void>(runner.render(formulas[0]));
        static_cast<void>(runner.render(formulas[1]));
        for (int i = 0; i < 60; ++i) {
            durations.push_back(runner.render(formulas[static_cast<std::size_t>(i) % 2]));
        }
        reportLatency("cached_repeat", durations);

        durations.clear();
        const int completionsBeforeRapid = runner.completions();
        for (int burst = 0; burst < 30; ++burst) {
            QElapsedTimer elapsed;
            elapsed.start();
            for (int edit = 0; edit < 20; ++edit) {
                runner.enqueue(formulas[0] + QLatin1Char('+') + QString::number(burst * 20 + edit));
            }
            runner.wait();
            durations.push_back(elapsed.nsecsElapsed());
        }
        reportLatency("rapid_edits_latest", durations);
        std::cout << "rapid_edits_requests=600 completions="
                  << runner.completions() - completionsBeforeRapid << '\n';
        reportMemory("after_rapid_edits");

        const auto wrappingFormula = QStringLiteral("x^2 + ").repeated(20) + QStringLiteral("y^2");
        durations.clear();
        for (int resize = 0; resize < 60; ++resize) {
            durations.push_back(runner.render(wrappingFormula, QSize(320 + resize * 4, 360)));
        }
        reportLatency("viewport_resize_uncached", durations);
        durations.clear();
        static_cast<void>(runner.render(wrappingFormula, QSize(320, 360)));
        static_cast<void>(runner.render(wrappingFormula, QSize(480, 360)));
        for (int resize = 0; resize < 60; ++resize) {
            durations.push_back(runner.render(wrappingFormula, QSize(resize % 2 ? 320 : 480, 360)));
        }
        reportLatency("viewport_resize_back_cached", durations);
        reportMemory("after_viewport_resize");

        runner.clear();
        reportMemory("after_cache_clear");
        std::cout << "total_requests=" << runner.requests() << " delivered=" << runner.completions()
                  << " stale_deliveries=" << runner.stale() << '\n';
        if (runner.stale() != 0) {
            throw std::runtime_error("Formula benchmark delivered a stale request");
        }
        // PNG export is outside every measured interval.
        if (!previewOutput.isEmpty()) {
            QImage exportImage(previewImage.size(), QImage::Format_ARGB32_Premultiplied);
            exportImage.setDevicePixelRatio(previewImage.devicePixelRatio());
            exportImage.fill(Qt::white);
            {
                QPainter painter(&exportImage);
                painter.drawImage(QPoint(), previewImage);
            }
            if (!exportImage.save(previewOutput, "PNG")) {
                throw std::runtime_error("Unable to save the formula preview PNG");
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
