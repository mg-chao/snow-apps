#include "snow_shot/presentation/screenshotrecognitionsessioncontroller.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/screenshottableeditor.h"
#include "snow_shot/presentation/screenshottoolpalette.h"
#include "snow_shot/storage/applicationstorage.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontDatabase>
#include <QJsonArray>
#include <QPointer>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {
using Mode = ScreenshotRecognitionSessionController::Mode;
using Tool = ScreenshotToolPalette::Tool;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct Samples {
    std::vector<double> operation;
    std::vector<double> heartbeat;
};

template <typename Operation> void sample(Samples& samples, Operation operation) {
    QEventLoop loop;
    QElapsedTimer timer;
    timer.start();
    QTimer::singleShot(0, &loop, [&] {
        samples.heartbeat.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
        loop.quit();
    });
    operation();
    samples.operation.push_back(static_cast<double>(timer.nsecsElapsed()) / 1e6);
    loop.exec();
}

double percentile(std::vector<double> values, double fraction) {
    require(!values.empty(), "benchmark must collect timing samples");
    std::sort(values.begin(), values.end());
    const auto index =
        static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size() - 1)));
    return values[index];
}

void report(const char* scenario, const Samples& samples) {
    std::cout << "scenario=" << scenario << " samples=" << samples.operation.size()
              << " operation_p50_ms=" << percentile(samples.operation, 0.50)
              << " operation_p95_ms=" << percentile(samples.operation, 0.95)
              << " next_gui_heartbeat_p50_ms=" << percentile(samples.heartbeat, 0.50)
              << " next_gui_heartbeat_p95_ms=" << percentile(samples.heartbeat, 0.95) << '\n';
}

ScreenshotToolPalette::Options paletteOptions() {
    ScreenshotToolPalette::Options options;
    options.showSelectTool = false;
    options.showShapeTool = false;
    options.showArrowTool = false;
    options.showDistanceTool = false;
    options.showAngleTool = false;
    options.showTableTool = true;
    options.showImageConversionTools = true;
    return options;
}

QVector<SnowShotChatModel> modelCatalog() {
    return {{QStringLiteral("benchmark-a"), QStringLiteral("Benchmark A"), false,
             QStringLiteral("default"), true},
            {QStringLiteral("benchmark-b"), QStringLiteral("Benchmark B"), false,
             QStringLiteral("default"), true}};
}

void preparePalette(ScreenshotToolPalette& palette) {
    const ScreenshotRecognitionModelState state{
        QStringLiteral("benchmark-a"), modelCatalog(), false, {}};
    palette.setRecognitionModelState(Tool::Table, state);
    palette.setRecognitionModelState(Tool::Latex, state);
    palette.setTableEnabled(true);
    palette.setLatexState(true, false);
    palette.resize(600, palette.sizeHint().height());
    palette.show();
    QCoreApplication::processEvents();
}

void benchmarkToolbarActivation() {
    for (Tool tool : {Tool::Table, Tool::Latex}) {
        Samples cold;
        Samples warm;
        for (int iteration = 0; iteration < 30; ++iteration) {
            ScreenshotToolPalette palette(paletteOptions());
            preparePalette(palette);
            sample(cold, [&] { palette.setActiveTool(tool); });
            for (int repeat = 0; repeat < 10; ++repeat) {
                palette.clearActiveTool();
                sample(warm, [&] { palette.setActiveTool(tool); });
            }
        }
        report(tool == Tool::Table ? "table_toolbar_cold_activation"
                                   : "latex_toolbar_cold_activation",
               cold);
        report(tool == Tool::Table ? "table_toolbar_warm_activation"
                                   : "latex_toolbar_warm_activation",
               warm);
    }
}

QString tableHtml(const QString& label) {
    QString html = QStringLiteral("<table>");
    for (int row = 0; row < 12; ++row) {
        html += QStringLiteral("<tr>");
        for (int column = 0; column < 6; ++column) {
            if (row == 0 && column == 0) {
                html += QStringLiteral("<th colspan=\"2\">%1 header</th>").arg(label);
                ++column;
            } else {
                html += QStringLiteral("<td>%1 row %2 column %3 content</td>")
                            .arg(label)
                            .arg(row)
                            .arg(column);
            }
        }
        html += QStringLiteral("</tr>");
    }
    return html + QStringLiteral("</table>");
}

void benchmarkCatalogUpdates() {
    for (const int count : {32, 1024}) {
        ScreenshotToolPalette palette(paletteOptions());
        preparePalette(palette);
        palette.setActiveTool(Tool::Table);
        ScreenshotRecognitionModelState state;
        state.models.reserve(count);
        for (int index = 0; index < count; ++index) {
            state.models.append({QStringLiteral("catalog-%1").arg(index),
                                 QStringLiteral("Catalog model %1").arg(index), false,
                                 QStringLiteral("default"), true});
        }
        state.selection = state.models.first().id;
        palette.setRecognitionModelState(Tool::Table, state);
        QCoreApplication::processEvents();
        Samples changed;
        Samples unchanged;
        for (int iteration = 0; iteration < 30; ++iteration) {
            state.selection = iteration % 2 == 0 ? state.models.last().id : state.models.first().id;
            sample(changed, [&] { palette.setRecognitionModelState(Tool::Table, state); });
            sample(unchanged, [&] { palette.setRecognitionModelState(Tool::Table, state); });
        }
        const auto prefix = QStringLiteral("recognition_catalog_%1").arg(count);
        report((prefix + QStringLiteral("_selection_update")).toLatin1().constData(), changed);
        report((prefix + QStringLiteral("_unchanged_state")).toLatin1().constData(), unchanged);
    }
}

void benchmarkCachedModelSwitches() {
    QWidget host;
    host.resize(480, 240);
    auto* layout = new QVBoxLayout(&host);
    auto* palette = new ScreenshotToolPalette(paletteOptions(), &host);
    auto* content = new ScreenshotRecognitionWindow(
        {}, &host, ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
    layout->addWidget(palette);
    layout->addWidget(content, 1);
    preparePalette(*palette);
    host.show();

    ScreenshotRecognitionSessionActions actions;
    actions.ensureContent = [content] { return content; };
    actions.setActiveMode = [palette](int mode) {
        if (mode == static_cast<int>(Mode::Table))
            palette->setActiveTool(Tool::Table);
        else if (mode == static_cast<int>(Mode::Latex))
            palette->setActiveTool(Tool::Latex);
        else
            palette->clearActiveTool();
    };
    actions.setRecognitionModelState = [palette](int mode, ScreenshotRecognitionModelState state) {
        state.models = modelCatalog();
        palette->setRecognitionModelState(
            mode == static_cast<int>(Mode::Table) ? Tool::Table : Tool::Latex, state);
    };
    actions.setTableEditingState = [palette](bool available, bool undo, bool redo, bool merge,
                                             bool split, bool reset) {
        palette->setTableEditingState(available, undo, redo, merge, split, reset);
    };
    actions.setLatexEditingState = [palette](bool available, bool undo, bool redo) {
        palette->setLatexEditingState(available, undo, redo);
    };
    actions.setBusyState = [palette](bool, bool table, bool) { palette->setTableBusy(table); };
    ScreenshotRecognitionSessionController session(nullptr, nullptr, nullptr, actions);
    QObject::connect(palette, &ScreenshotToolPalette::recognitionModelChanged, &session,
                     &ScreenshotRecognitionSessionController::setRecognitionModel);
    QImage image(480, 240, QImage::Format_RGB32);
    image.fill(Qt::white);
    session.setTarget({QStringLiteral("benchmark"), image, QRectF(0, 0, 480, 240)});
    ScreenshotRecognitionResults results;
    results.key = QStringLiteral("benchmark");
    results.tableModelSelection = results.latexModelSelection = QStringLiteral("benchmark-a");
    for (const auto& model : modelCatalog()) {
        ScreenshotTableRecognitionEntry table;
        table.model = model.id;
        table.result.html = tableHtml(model.name);
        results.tableEntries.append(table);
        ScreenshotLatexRecognitionEntry latex;
        latex.model = model.id;
        latex.result.latex = model.id.endsWith(u'a') ? QStringLiteral("\\frac{a}{b}")
                                                     : QStringLiteral("\\sqrt{x+1}");
        results.latexEntries.append(latex);
    }
    session.seedRecognitionResults(results);
    session.activate(Mode::Table);
    QCoreApplication::processEvents();
    QPointer<ScreenshotTableEditor> tableEditor = content->findChild<ScreenshotTableEditor*>();
    require(tableEditor && tableEditor->columnSpan(0, 0) == 2 && !session.busy(),
            "table benchmark must use a cached real editor with merged cells");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_cell")},
                                  {QStringLiteral("row"), 4},
                                  {QStringLiteral("column"), 2},
                                  {QStringLiteral("text"), QStringLiteral("Edited A")}}),
            "table benchmark creates model A edit history");
    tableEditor->setCurrentIndex(tableEditor->model()->index(4, 2));
    tableEditor->verticalScrollBar()->setValue(64);
    tableEditor->horizontalScrollBar()->setValue(20);
    const int verticalScroll = tableEditor->verticalScrollBar()->value();
    const int horizontalScroll = tableEditor->horizontalScrollBar()->value();
    const QJsonObject editedTable = session.workflowResult();
    session.setRecognitionModel(QStringLiteral("benchmark-b"));
    require(tableEditor &&
                tableEditor->model()->data(tableEditor->model()->index(0, 0)).toString() ==
                    QStringLiteral("Benchmark B header") &&
                !session.busy(),
            "model B must also reopen from a cached table result");
    require(session.editWorkflow({{QStringLiteral("action"), QStringLiteral("set_cell")},
                                  {QStringLiteral("row"), 6},
                                  {QStringLiteral("column"), 3},
                                  {QStringLiteral("text"), QStringLiteral("Edited B")}}),
            "table benchmark creates independent model B edit history");
    const QJsonObject editedTableB = session.workflowResult();
    session.setRecognitionModel(QStringLiteral("benchmark-a"));
    QCoreApplication::processEvents();
    for (int warmup = 0; warmup < 20; ++warmup) {
        session.setRecognitionModel(QStringLiteral("benchmark-b"));
        session.setRecognitionModel(QStringLiteral("benchmark-a"));
    }
    QCoreApplication::processEvents();
    Samples tableSamples;
    for (int iteration = 0; iteration < 200; ++iteration) {
        sample(tableSamples, [&] { session.setRecognitionModel(QStringLiteral("benchmark-b")); });
        sample(tableSamples, [&] { session.setRecognitionModel(QStringLiteral("benchmark-a")); });
    }
    require(tableEditor && content->findChild<ScreenshotTableEditor*>() == tableEditor &&
                session.workflowResult() == editedTable && tableEditor->columnSpan(0, 0) == 2 &&
                tableEditor->verticalScrollBar()->value() == verticalScroll &&
                tableEditor->horizontalScrollBar()->value() == horizontalScroll &&
                tableEditor->currentIndex().row() == 4 &&
                tableEditor->currentIndex().column() == 2 && !session.busy(),
            "cached switches must retain table editor, edits, spans, current cell and scrolling");
    report("table_cached_model_switch", tableSamples);
    session.setRecognitionModel(QStringLiteral("benchmark-b"));
    require(session.workflowResult() == editedTableB,
            "cached table switches retain model B's independent edit history");

    session.activate(Mode::Latex);
    session.setTextDraft(QStringLiteral("\\frac{a+1}{b}"));
    QCoreApplication::processEvents();
    auto* latexEditor = content->findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(latexEditor && !session.busy(), "LaTeX benchmark must use the real source editor");
    QTextDocument* documentA = latexEditor->document();
    session.setRecognitionModel(QStringLiteral("benchmark-b"));
    require(session.latexDraft() == QStringLiteral("\\sqrt{x+1}") && !session.busy(),
            "model B must also reopen from a cached LaTeX result");
    session.setTextDraft(QStringLiteral("\\sqrt{x+2}"));
    QTextDocument* documentB = latexEditor->document();
    require(documentA != documentB, "each LaTeX model owns its own editing document");
    session.setRecognitionModel(QStringLiteral("benchmark-a"));
    for (int warmup = 0; warmup < 20; ++warmup) {
        session.setRecognitionModel(QStringLiteral("benchmark-b"));
        session.setRecognitionModel(QStringLiteral("benchmark-a"));
    }
    QCoreApplication::processEvents();
    Samples latexSamples;
    for (int iteration = 0; iteration < 200; ++iteration) {
        sample(latexSamples, [&] { session.setRecognitionModel(QStringLiteral("benchmark-b")); });
        sample(latexSamples, [&] { session.setRecognitionModel(QStringLiteral("benchmark-a")); });
    }
    require(content->findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor")) == latexEditor &&
                latexEditor->document() == documentA &&
                session.latexDraft() == QStringLiteral("\\frac{a+1}{b}") && !session.busy(),
            "cached LaTeX switches retain the source editor and model A editing document");
    session.setRecognitionModel(QStringLiteral("benchmark-b"));
    require(latexEditor->document() == documentB &&
                session.latexDraft() == QStringLiteral("\\sqrt{x+2}"),
            "cached LaTeX switches retain model B's independent editing document");
    report("latex_cached_model_switch", latexSamples);
    std::cout << "cached_editors_retained=true provider=null network_requests=0\n";
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    std::cout.setf(std::ios::unitbuf);
#ifndef NDEBUG
    std::cerr << "Run with windows-msvc-performance Release.\n";
    return EXIT_FAILURE;
#else
    QTemporaryDir temporary;
    require(temporary.isValid(), "benchmark requires isolated temporary settings");
    const QString executable = temporary.filePath(QStringLiteral("bin"));
    require(QDir().mkpath(executable), "benchmark executable directory is created");
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({executable, temporary.path(), 60000})
                .success,
            "benchmark initializes isolated settings with deferred disk writes");
#if defined(Q_OS_WIN)
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >= 0,
            "offscreen toolbar benchmarks require a system TrueType font");
#endif
    std::cout << std::fixed << std::setprecision(3)
              << "build=Release platform=" << application.platformName().toStdString() << '\n';
    benchmarkToolbarActivation();
    benchmarkCatalogUpdates();
    benchmarkCachedModelSwitches();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return EXIT_SUCCESS;
#endif
}
