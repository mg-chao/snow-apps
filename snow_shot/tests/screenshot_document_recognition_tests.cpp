#include "snow_shot/presentation/screenshotrecognizeddocument.h"
#include "snow_shot/presentation/screenshotrecognitionfileexport.h"
#include "snow_shot/presentation/screenshotlatexrenderer.h"
#include "recognition_workbook_test_support.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QTextDocument>
#include <QTextTable>
#include <QXmlStreamReader>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "output readable");
    return file.readAll();
}
QImage renderFormula(const QString& source) {
    ScreenshotLatexRenderer renderer;
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QImage raster;
    auto status = ScreenshotLatexRenderer::Error::InitializationFailed;
    QObject::connect(&renderer, &ScreenshotLatexRenderer::rendered, &loop,
                     [&](quint64, const QImage& image, ScreenshotLatexRenderer::Error error) {
                         raster = image;
                         status = error;
                         loop.quit();
                     });
    deadline.start(10000);
    static_cast<void>(renderer.request(source, QSize(1200, 800), 2, Qt::black));
    loop.exec();
    require(status == ScreenshotLatexRenderer::Error::None && !raster.isNull(),
            "document formulas render using the existing preview renderer");
    require(raster.devicePixelRatio() == 2, "formula raster preserves high-DPI rendering");
    return raster;
}

void runSerializationBenchmark() {
    QImage source(3840, 2160, QImage::Format_RGB32);
    for (int y = 0; y < source.height(); ++y) {
        auto* row = reinterpret_cast<QRgb*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x)
            row[x] = qRgb((x + y) % 256, (x * y) % 256, (x ^ y) % 256);
    }
    QString markdown;
    for (int region = 0; region < 8; ++region) {
        markdown += QStringLiteral("### Figure %1\n\n![](page=0,bbox=[%2,%3,%4,%5])\n\n")
                        .arg(region)
                        .arg(region % 4 * 960)
                        .arg(region / 4 * 1080)
                        .arg((region % 4 + 1) * 960)
                        .arg((region / 4 + 1) * 1080);
        markdown +=
            QStringLiteral("Text accompanying the figure. ").repeated(100) + QStringLiteral("\n\n");
    }
    QElapsedTimer timer;
    timer.start();
    const auto document = ScreenshotRecognizedDocument::parse(
        {{QStringLiteral("md_results"), markdown}}, source, nullptr);
    require(document && document->images.size() == 8, "prepare serialization benchmark document");
    const auto parseMs = timer.nsecsElapsed();
    timer.restart();
    document->preparedClipboard = document->clipboardContent();
    const auto prepareMs = timer.nsecsElapsed();
    auto selection = *document;
    selection.preparedClipboard.reset();
    selection.html =
        QStringLiteral("<p>First figure</p><p><img src=\"snow-document-image-0\" /></p>");
    timer.restart();
    const auto selected = selection.clipboardContent();
    const auto selectionMs = timer.nsecsElapsed();
    timer.restart();
    for (int i = 0; i < 1000; ++i)
        require(document->clipboardContent().html.size() ==
                    document->preparedClipboard->html.size(),
                "cached clipboard retains the full document");
    const auto cachedMs = timer.nsecsElapsed();
    require(!selected.html.isEmpty() && !selected.text.isEmpty() &&
                selected.html.size() < document->preparedClipboard->html.size(),
            "selection serializes only the referenced crop");
    std::cout << "document_4k_8_figures parse_ms=" << double(parseMs) / 1e6
              << " prepare_ms=" << double(prepareMs) / 1e6
              << " selection_ms=" << double(selectionMs) / 1e6
              << " cached_copy_average_ms=" << double(cachedMs) / 1e9
              << " retained_bytes=" << document->retainedBytes() << '\n';
}
} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const auto fontRoot = QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
                              .filePath(QStringLiteral("../../test-support/fonts/"));
    for (const auto* name : {"SnowRecordingTestSans-Regular.ttf", "SnowRecordingTestSans-Bold.ttf",
                             "SnowRecordingTestHan-Regular.ttf", "SnowRecordingTestHan-Bold.ttf"}) {
        const int font =
            QFontDatabase::addApplicationFont(QDir(fontRoot).filePath(QString::fromLatin1(name)));
        if (font >= 0 && QByteArray(name).contains("Sans-Regular"))
            app.setFont(QFont(QFontDatabase::applicationFontFamilies(font).first(), 11));
    }
    QFontDatabase::addApplicationFallbackFontFamily(QChar::Script_Han,
                                                    QStringLiteral("Snow Recording Test Han"));
    try {
        if (app.arguments().contains(QStringLiteral("--serialization-benchmark"))) {
            runSerializationBenchmark();
            return 0;
        }
        QImage original(200, 100, QImage::Format_ARGB32);
        original.fill(Qt::green);
        original.setPixelColor(40, 20, Qt::red);
        const QJsonObject raw{
            {QStringLiteral("md_results"),
             QString::fromUtf8(
                 "# Report 汉字\n\nParagraph **bold** $x^2$.\n\n"
                 "$$\\frac{a}{b}$$\n\n"
                 "1. First\n   - Nested\n2. Second\n\n"
                 "<table><tr><th colspan=\"2\">Merged</th></tr>"
                 "<tr><td rowspan=\"2\">A</td><td>B</td></tr><tr><td>C</td></tr></table>\n\n"
                 "![](page=0,bbox=[20,10,60,30])\n\n"
                 "![blocked](file:///C:/Windows/private.png)\n")},
            {QStringLiteral("data_info"),
             QJsonObject{{QStringLiteral("pages"),
                          QJsonArray{QJsonObject{{QStringLiteral("width"), 100},
                                                 {QStringLiteral("height"), 50}}}}}}};
        QString error;
        auto document = ScreenshotRecognizedDocument::parse(raw, original, &error);
        require(document != nullptr && error.isEmpty(), "fixture parsed");
        require(document->images.size() == 1, "only authorized crop retained");
        const auto figureId = document->images.constBegin().key();
        const auto crop = document->images.constBegin().value();
        require(crop.size() == QSize(80, 40) && crop.pixelColor(0, 0) == Qt::red,
                "page coordinates mapped to original pixels");
        const auto referenced = QJsonDocument::fromJson(R"JSON({
            "md_results": "![Figure](https://provider.invalid/figure.png)",
            "layout_details": [[{"label":"image", "image_path":"https://provider.invalid/figure.png", "bbox_2d":[0.2,0.2,0.6,0.6]}]]
    })JSON")
                                    .object();
        const auto resolved = ScreenshotRecognizedDocument::parse(referenced, original, &error);
        require(resolved && resolved->images.size() == 1 &&
                    resolved->images.constBegin().value() == crop,
                "layout image references resolve to normalized local crops without downloads");
        require(document->formulas.size() == 2 &&
                    document->formulas[0].source == QStringLiteral("x^2") &&
                    !document->formulas[0].display && document->formulas[1].display,
                "inline and display formula sources preserved");
        for (const auto& formula : document->formulas)
            document->images.insert(formula.id, renderFormula(formula.source));
        require(document->plainText().contains(QStringLiteral("x^2")),
                "plain text retains mathematics");
        require(!document->portableHtml().contains(QStringLiteral("file:///")),
                "untrusted local resource removed");
        require(document->portableHtml().contains(QStringLiteral("data:image/png;base64,")),
                "HTML embeds assets");
        require(document->html.contains(QStringLiteral("colspan=\"2\"")), "merged table preserved");
        require(
            document->markdown(QStringLiteral("report_images")).contains(QStringLiteral("$x^2$")),
            "Markdown preserves math source");
        const auto latex = document->latex(QStringLiteral("report_images"));
        require(latex.contains(QStringLiteral("\\begin{document}")) &&
                    latex.contains(QStringLiteral("report_images/snow-document-formula-0.png")) &&
                    latex.contains(QStringLiteral("\\begin{center}\\includegraphics[width=")) &&
                    latex.contains(QStringLiteral("\n% x^2\n")) &&
                    latex.contains(QStringLiteral("\n% \\frac{a}{b}\n")),
                "LaTeX embeds formula rasters and preserves inert sources");
        const auto formulaWidth = QStringLiteral("width=%1pt")
                                      .arg(document->images.value(document->formulas[0].id)
                                                   .deviceIndependentSize()
                                                   .width() *
                                               0.75,
                                           0, 'f', 2);
        require(latex.contains(formulaWidth),
                "inline formula retains its logical size instead of expanding to page width");
        require(!ScreenshotRecognizedDocument::parse({}, original, &error),
                "malformed provider result rejected");
        QTemporaryDir output;
        ScreenshotRecognitionFileSnapshot snapshot;
        snapshot.kind = ScreenshotRecognitionFileKind::DocumentWord;
        snapshot.document = document;
        const auto mdPath = output.filePath(QString::fromUtf8("文档.md"));
        bool dispatched = false;
        QTimer::singleShot(0, &app, [&] { dispatched = true; });
        const auto savedMarkdown = ScreenshotRecognitionFileExport::saveToPath(snapshot, mdPath);
        require(savedMarkdown.succeeded(), qPrintable(savedMarkdown.error));
        require(!dispatched, "document writer never dispatches a nested application event loop");
        QCoreApplication::processEvents();
        const auto markdown = read(mdPath);
        const auto figureName = figureId + QStringLiteral(".png");
        const auto imageDirectoryName = QString::fromUtf8("文档.md_images");
        const QDir markdownImages(output.filePath(imageDirectoryName));
        require(markdown.contains("$x^2$") && markdown.contains("$$\\frac{a}{b}$$") &&
                    markdown.contains(QUrl::toPercentEncoding(imageDirectoryName) + '/' +
                                      figureName.toUtf8()),
                "saved Markdown retains both formula sources and the figure reference");
        require(markdown.startsWith("# Report ") && markdown.contains("**bold**") &&
                    markdown.contains("\n\n<table") && markdown.contains("colspan=\"2\"") &&
                    markdown.contains("rowspan=\"2\"") && !markdown.contains("<body"),
                "Markdown keeps surrounding text and math outside the merged table's HTML");
        require(markdownImages.entryList(QDir::AllEntries | QDir::NoDotAndDotDot) ==
                    QStringList{figureName},
                "Markdown writes only the figure to its image directory");
        require(QImage::fromData(read(markdownImages.filePath(figureName)), "PNG")
                        .convertToFormat(QImage::Format_ARGB32) == crop,
                "the Markdown figure preserves the original crop");

        auto figureSnapshot = snapshot;
        figureSnapshot.document = resolved;
        const auto figurePath = output.filePath(QStringLiteral("figure.md"));
        const auto savedFigure =
            ScreenshotRecognitionFileExport::saveToPath(figureSnapshot, figurePath);
        require(savedFigure.succeeded(), qPrintable(savedFigure.error));
        const auto figureReference = QStringLiteral("figure.md_images/") + figureName;
        require(read(figurePath)
                        .contains(QStringLiteral("![Figure](%1)").arg(figureReference).toUtf8()) &&
                    QFileInfo::exists(output.filePath(figureReference)),
                "a figure is also linked and saved in a document without merged cells");

        const auto formulasOnly = ScreenshotRecognizedDocument::parse(
            QJsonObject{{QStringLiteral("md_results"),
                         QStringLiteral("$x^2$\n\n$$\\left[\\frac{a}{b}\\right]$$")}},
            original, &error);
        require(formulasOnly && formulasOnly->formulas.size() == 2, "formula-only fixture parsed");
        for (const auto& formula : formulasOnly->formulas)
            formulasOnly->images.insert(formula.id, crop);
        auto formulasOnlySnapshot = snapshot;
        formulasOnlySnapshot.kind = ScreenshotRecognitionFileKind::DocumentMarkdown;
        formulasOnlySnapshot.document = formulasOnly;
        const auto formulasPath = output.filePath(QStringLiteral("formulas.md"));
        const auto savedFormulas =
            ScreenshotRecognitionFileExport::saveToPath(formulasOnlySnapshot, formulasPath);
        require(savedFormulas.succeeded(), qPrintable(savedFormulas.error));
        const auto formulasMarkdown = read(formulasPath);
        require(formulasMarkdown.contains("$x^2$") &&
                    formulasMarkdown.contains("$$\\left[\\frac{a}{b}\\right]$$") &&
                    !QFileInfo::exists(output.filePath(QStringLiteral("formulas.md_images"))),
                "formula-only Markdown saves source without an image directory");
        const auto existingImages = output.filePath(QStringLiteral("existing.md_images"));
        QFile existingFile(existingImages);
        require(existingFile.open(QIODevice::WriteOnly) && existingFile.write("keep") == 4,
                "unrelated file at the potential image directory path");
        existingFile.close();
        const auto savedAlongside = ScreenshotRecognitionFileExport::quickSave(
            formulasOnlySnapshot, output.path(), QStringLiteral("existing"));
        require(savedAlongside.succeeded() &&
                    savedAlongside.path == output.filePath(QStringLiteral("existing.md")) &&
                    read(existingImages) == "keep",
                "quick save uses the requested Markdown name and leaves unrelated files alone");

        for (const auto& extension : {"docx", "pdf", "html"}) {
            const auto path =
                output.filePath(QString::fromUtf8("文档.") + QString::fromLatin1(extension));
            const auto saved = ScreenshotRecognitionFileExport::saveToPath(snapshot, path, true);
            require(saved.succeeded(), qPrintable(saved.error));
            require(!read(path).isEmpty(), "export contains bytes");
        }
        const auto texPath = output.filePath(QStringLiteral("latex.tex"));
        const auto savedLatex = ScreenshotRecognitionFileExport::saveToPath(snapshot, texPath);
        require(savedLatex.succeeded(), qPrintable(savedLatex.error));
        const auto latexFile = read(texPath);
        for (const auto& formula : document->formulas) {
            const auto reference =
                QStringLiteral("latex.tex_images/") + formula.id + QStringLiteral(".png");
            require(latexFile.contains(reference.toUtf8()) &&
                        QFileInfo::exists(output.filePath(reference)),
                    "LaTeX exports each referenced formula raster");
        }
        const auto sameStemPath = output.filePath(QString::fromUtf8("文档.tex"));
        const auto savedSameStem =
            ScreenshotRecognitionFileExport::saveToPath(formulasOnlySnapshot, sameStemPath, true);
        require(savedSameStem.succeeded() &&
                    QFileInfo::exists(output.filePath(QString::fromUtf8("文档.tex_images"))) &&
                    QFileInfo::exists(markdownImages.filePath(figureName)) &&
                    read(mdPath) == markdown,
                "saving LaTeX with the same stem preserves Markdown's content and figures");
        require(!ScreenshotRecognitionFileExport::saveToPath(snapshot, {}).succeeded(),
                "a document save without a destination is rejected");
        const auto package = recognition_workbook_test::parts(
            output.filePath(QString::fromUtf8("文档.docx")), false);
        const auto word = package.value(QStringLiteral("word/document.xml"));
        require(word.contains("<w:t") && word.contains("Report") && word.contains("<w:numPr>") &&
                    word.contains("<w:gridSpan w:val=\"2\"/>") &&
                    word.contains("<w:vMerge w:val=\"restart\"/>") && word.contains("<w:vMerge/>"),
                "Word preserves editable text, lists and merged tables");
        require(word.count("<w:drawing>") == 3 && !word.contains("<m:oMath") &&
                    !word.contains("officeDocument/2006/math") && word.contains("descr=\"x^2\"") &&
                    word.contains("descr=\"\\frac{a}{b}\""),
                "Word embeds inline and display formulas as images with source descriptions");
        const auto sources = package.value(QStringLiteral("customXml/item1.xml"));
        require(sources.contains("x^2") && sources.contains("\\frac{a}{b}"),
                "Word retains original formula sources");
        const auto numbering = package.value(QStringLiteral("word/numbering.xml"));
        require(numbering.contains("<w:lvlText w:val=\"%1.\"/>") &&
                    numbering.contains("w:left=\"360\"") && !numbering.contains("%5"),
                "Word list numbering retains its native placeholder and numeric indentation");
        int embeddedImages = 0;
        QXmlStreamReader relationships(
            package.value(QStringLiteral("word/_rels/document.xml.rels")));
        while (!relationships.atEnd()) {
            relationships.readNext();
            if (!relationships.isStartElement() || relationships.name() != u"Relationship" ||
                !relationships.attributes().value(u"Type").endsWith(u"/image"))
                continue;
            const auto id = relationships.attributes().value(u"Id").toString();
            const auto path =
                QStringLiteral("word/") + relationships.attributes().value(u"Target").toString();
            const auto image = QImage::fromData(package.value(path), "PNG");
            require(!image.isNull() &&
                        word.contains(QStringLiteral("r:embed=\"%1\"").arg(id).toUtf8()),
                    "Word drawings resolve to embedded PNGs");
            bool matchesAsset = false;
            for (const auto& asset : document->images)
                matchesAsset = matchesAsset || image.convertToFormat(QImage::Format_ARGB32) ==
                                                   asset.convertToFormat(QImage::Format_ARGB32);
            require(matchesAsset,
                    "Word embeds original figure and rendered formula pixels losslessly");
            ++embeddedImages;
        }
        require(!relationships.hasError() && embeddedImages == 3,
                "Word embeds the figure and both rendered formulas");
        const auto artifactDirectory = qEnvironmentVariable("SNOW_DOCUMENT_TEST_ARTIFACTS");
        if (!artifactDirectory.isEmpty()) {
            QDir().mkpath(artifactDirectory);
            for (const auto& extension : {"docx", "pdf", "md", "html"}) {
                const auto name = QStringLiteral("document.") + QString::fromLatin1(extension);
                QFile::copy(
                    output.filePath(QString::fromUtf8("文档.") + QString::fromLatin1(extension)),
                    QDir(artifactDirectory).filePath(name));
            }
            QFile::copy(texPath, QDir(artifactDirectory).filePath(QStringLiteral("document.tex")));
        }
        const auto& before = markdown;
        const auto cancelled = ScreenshotRecognitionFileExport::saveToPath(snapshot, mdPath, true,
                                                                           [] { return true; });
        require(cancelled.wasCancelled && read(mdPath) == before,
                "cancel preserves prior publication");
        auto changed = std::make_shared<ScreenshotRecognizedDocument>(*document);
        changed->html.replace(QStringLiteral("Paragraph"), QStringLiteral("Replacement content"));
        auto replacement = snapshot;
        replacement.document = changed;
        const auto rolledBack =
            ScreenshotRecognitionFileExport::saveToPath(replacement, mdPath, true, [&] {
                return QFile::exists(mdPath) && read(mdPath) != before;
            });
        require(rolledBack.wasCancelled && read(mdPath) == before,
                "cancellation between primary and sidecar publication rolls back the primary");
        require(!ScreenshotRecognitionFileExport::saveToPath(snapshot, mdPath).succeeded(),
                "overwrite requires authorization");
        require(document->markdown(QStringLiteral("test_images")).contains(QStringLiteral("$x^2$")),
                "Markdown retains formula source with or without a raster");
        bool launched = false;
        auto opened =
            ScreenshotRecognitionFileExport::openTemporaryDocument(snapshot, [&](const QUrl& url) {
                launched = url.isLocalFile() && QFile::exists(url.toLocalFile());
                return false;
            });
        require(launched && !opened.succeeded(), "launcher failure reported without success");
        QString launchedPath;
        opened =
            ScreenshotRecognitionFileExport::openTemporaryDocument(snapshot, [&](const QUrl& url) {
                launchedPath = url.toLocalFile();
                return QFile::exists(launchedPath);
            });
        require(opened.succeeded() && QFile::exists(launchedPath),
                "successful handoff retains the document");
        // Only the unique task-owned temporary directory is removed by this fixture.
        require(QDir(QFileInfo(launchedPath).absolutePath()).removeRecursively(),
                "cleanup retained fixture");
        std::cout << "Document recognition tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
