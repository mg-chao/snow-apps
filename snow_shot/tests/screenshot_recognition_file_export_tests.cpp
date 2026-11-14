#include "snow_shot/presentation/screenshotrecognitionfileexport.h"
#include "recognition_workbook_test_support.h"

#include <QApplication>
#include <QAbstractButton>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QTranslator>
#include <QUrl>
#include <QtEndian>

#include <stdexcept>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QByteArray read(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "saved text file is unreadable");
    return file.readAll();
}

void write(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "text fixture cannot be written");
    require(file.write(bytes) == bytes.size(), "text fixture write failed");
}

void requireClassicZipWorkbook(const QString& path) {
    // Read the ZIP records directly: minizip accepts ZIP64 local headers that older Excel
    // versions reject, even when the central directory only contains 32-bit sizes.
    const QByteArray bytes = read(path);
    const auto word = [&](qsizetype offset) {
        require(offset >= 0 && offset + 2 <= bytes.size(), "truncated ZIP word");
        return qFromLittleEndian<quint16>(bytes.constData() + offset);
    };
    const auto dword = [&](qsizetype offset) {
        require(offset >= 0 && offset + 4 <= bytes.size(), "truncated ZIP dword");
        return qFromLittleEndian<quint32>(bytes.constData() + offset);
    };
    const auto checkExtraFields = [&](qsizetype offset, quint16 length) {
        const qsizetype end = offset + length;
        require(end <= bytes.size(), "truncated ZIP extra fields");
        while (offset < end) {
            require(offset + 4 <= end && word(offset) != 0x0001,
                    "Excel workbook entries must not require ZIP64 extra fields");
            offset += 4 + word(offset + 2);
            require(offset <= end, "invalid ZIP extra field length");
        }
    };

    const qsizetype end = bytes.lastIndexOf(QByteArray::fromHex("504b0506"));
    require(end >= 0 && end + 22 + word(end + 20) == bytes.size(),
            "workbook must end with a classic ZIP end record");
    const quint16 count = word(end + 10);
    require(word(end + 4) == 0 && word(end + 6) == 0 && count > 0 && count < 0xffff &&
                word(end + 8) == count,
            "workbook must be a single-disk classic ZIP archive");
    qsizetype central = dword(end + 16);
    require(central + dword(end + 12) == end,
            "the central directory must be followed directly by the classic end record");
    qsizetype nextLocal = 0;
    for (quint16 index = 0; index < count; ++index) {
        require(dword(central) == 0x02014b50 && word(central + 6) <= 20,
                "workbook central directory must support ZIP 2.0 readers");
        const quint16 flags = word(central + 8);
        require((flags & ~quint16(0x080e)) == 0 && word(central + 10) == 8 &&
                    word(central + 34) == 0,
                "workbook parts must use unencrypted, single-disk DEFLATE");
        const quint32 crc = dword(central + 16);
        const quint32 compressed = dword(central + 20);
        const quint32 uncompressed = dword(central + 24);
        require(compressed < 0xffffffff && uncompressed < 0xffffffff,
                "workbook sizes must fit classic ZIP fields");
        const quint16 nameLength = word(central + 28);
        const quint16 extraLength = word(central + 30);
        checkExtraFields(central + 46 + nameLength, extraLength);
        const qsizetype local = dword(central + 42);
        require(local == nextLocal && dword(local) == 0x04034b50 && word(local + 4) <= 20,
                "workbook local headers must support ZIP 2.0 readers without ZIP64");
        require(word(local + 6) == flags && word(local + 8) == 8 &&
                    word(local + 26) == nameLength &&
                    bytes.mid(local + 30, nameLength) == bytes.mid(central + 46, nameLength),
                "local and central ZIP entry metadata must agree");
        const quint16 localExtraLength = word(local + 28);
        checkExtraFields(local + 30 + nameLength, localExtraLength);
        require(dword(local + 18) < 0xffffffff && dword(local + 22) < 0xffffffff,
                "local ZIP sizes must not use ZIP64 sentinels");
        nextLocal = local + 30 + nameLength + localExtraLength + compressed;
        if (flags & 0x0008) {
            require(dword(nextLocal) == 0x08074b50 && dword(nextLocal + 4) == crc &&
                        dword(nextLocal + 8) == compressed && dword(nextLocal + 12) == uncompressed,
                    "streamed workbook parts must have matching 32-bit data descriptors");
            nextLocal += 16;
        } else {
            require(dword(local + 14) == crc && dword(local + 18) == compressed &&
                        dword(local + 22) == uncompressed,
                    "non-streamed workbook parts must have matching CRC and sizes");
        }
        central += 46 + nameLength + extraLength + word(central + 32);
    }
    require(central == end && nextLocal == dword(end + 16),
            "all workbook ZIP entries must end at the central directory");
}

void cancellationLeavesExistingFilesAndCompanionsIntact() {
    QTemporaryDir directory;
    const auto primary = directory.filePath(QStringLiteral("result.md"));
    const auto companion = directory.filePath(QStringLiteral("result.txt"));
    const ScreenshotRecognitionFileSnapshot snapshot{ScreenshotRecognitionFileKind::Markdown,
                                                     QStringLiteral("replacement")};
    for (const int stopAt : {1, 2, 3, 4, 5}) {
        write(primary, "original primary");
        write(companion, "original companion");
        int checkpoints = 0;
        const auto result = ScreenshotRecognitionFileExport::saveToPath(
            snapshot, primary, true, [&] { return ++checkpoints == stopAt; });
        require(result.wasCancelled && !result.succeeded(),
                "cancellation is distinct from save failure");
        require(read(primary) == "original primary" && read(companion) == "original companion" &&
                    QDir(directory.path()).entryList(QDir::Files | QDir::Hidden).size() == 2,
                "cancelled staging cleans temporary files and preserves both original outputs");
    }
    const auto cancelled = ScreenshotRecognitionFileExport::quickSave(
        snapshot, directory.path(), QStringLiteral("new"), [] { return true; });
    require(cancelled.wasCancelled && !QFileInfo::exists(directory.filePath("new.md")),
            "cancelled quick save never publishes an output");
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    ScreenshotRecognitionFileSnapshot table;
    table.kind = ScreenshotRecognitionFileKind::Table;
    table.table = ScreenshotTableDocument(100, 10);
    table.table->setCellText(0, 0, QStringLiteral("value"));
    int checkpoints = 0;
    const auto workbook = ScreenshotRecognitionFileExport::saveToPath(
        table, directory.filePath("cancelled.xlsx"), false, [&] { return ++checkpoints >= 20; });
    require(workbook.wasCancelled && !QFileInfo::exists(directory.filePath("cancelled.xlsx")),
            "workbook serialization observes cancellation between rows");
#endif
}

void savesConversionPairsWithExactSource() {
    QTemporaryDir directory;
    require(directory.isValid(), "conversion save directory unavailable");
    for (const auto kind :
         {ScreenshotRecognitionFileKind::Html, ScreenshotRecognitionFileKind::Markdown}) {
        const QString source = kind == ScreenshotRecognitionFileKind::Html
                                   ? QStringLiteral("<h1>雪</h1>\n<p>Partial")
                                   : QStringLiteral("# 雪\n\n**Partial");
        const QString input = directory.filePath(QStringLiteral("result.other"));
        const auto saved = ScreenshotRecognitionFileExport::saveToPath({kind, source}, input, true);
        const QString primary = directory.filePath(
            QStringLiteral("result.") + ScreenshotRecognitionFileExport::extension(kind));
        require(saved.succeeded() && saved.path == primary,
                "conversion primary extension must match its source format");
        require(read(primary) == source.toUtf8() &&
                    read(directory.filePath(QStringLiteral("result.txt"))) == source.toUtf8(),
                "conversion primary and companion must preserve exact UTF-8 source");
    }
}

ScreenshotRecognitionFileSnapshot tableSnapshot() {
    ScreenshotRecognitionFileSnapshot snapshot;
    snapshot.kind = ScreenshotRecognitionFileKind::Table;
    snapshot.table = ScreenshotTableDocument::fromHtml(QStringLiteral(
        "<table><tr><th colspan=\"2\">Header</th><th rowspan=\"2\">Group</th></tr>"
        "<tr><td></td><td>42</td></tr><tr><td></td><td></td><td></td></tr></table>"));
    return snapshot;
}

void savesRealWorkbookWithLiteralCellsAndSpans() {
    QTemporaryDir directory;
    auto snapshot = tableSnapshot();
    const QString longNumber = QStringLiteral("00123456789012345678901234567890");
    snapshot.table->setCellText(1, 0, longNumber);
    snapshot.table->setCellText(1, 1, QStringLiteral("=SUM(A1:A2)"));
    snapshot.table->setCellText(2, 0, QStringLiteral("  雪 < & > \" ' 😀\nnext\tline  "));
    snapshot.table->setCellText(2, 1, QStringLiteral("_x0041_\r") + QChar(1));
    const auto saved = ScreenshotRecognitionFileExport::saveToPath(
        snapshot, directory.filePath(QStringLiteral("表格.png")));
    require(saved.succeeded() && saved.path.endsWith(QStringLiteral("表格.xlsx")),
            "table saves must normalize to an Excel extension");
    requireClassicZipWorkbook(saved.path);
    require(QDir(directory.path()).entryList(QDir::Files).size() == 1 &&
                ScreenshotRecognitionFileExport::outputPaths(saved.path, snapshot) ==
                    QStringList{saved.path},
            "table export must create one workbook without a text companion");
    const auto package = recognition_workbook_test::parts(saved.path);
    for (const auto* name :
         {"[Content_Types].xml", "_rels/.rels", "xl/workbook.xml", "xl/_rels/workbook.xml.rels",
          "xl/styles.xml", "xl/worksheets/sheet1.xml"})
        require(package.contains(QString::fromLatin1(name)), "workbook part is missing");
    require(package.value(QStringLiteral("_rels/.rels")).contains("Target=\"xl/workbook.xml\"") &&
                package.value(QStringLiteral("xl/_rels/workbook.xml.rels"))
                    .contains("Target=\"worksheets/sheet1.xml\"") &&
                package.value(QStringLiteral("[Content_Types].xml"))
                    .contains("spreadsheetml.sheet.main+xml"),
            "package relationships and content types must describe an Excel workbook");
    const auto sheet = recognition_workbook_test::sheet(package);
    require(sheet.dimension == QStringLiteral("A1:C3") && sheet.cells.size() == 7 &&
                sheet.merges == QStringList{QStringLiteral("A1:B1"), QStringLiteral("C1:C2")} &&
                !sheet.cells.contains(QStringLiteral("B1")) &&
                !sheet.cells.contains(QStringLiteral("C2")),
            "dimensions and merge ranges must preserve table geometry without duplicate cells");
    require(sheet.cells.value(QStringLiteral("A2")) == longNumber &&
                sheet.cells.value(QStringLiteral("B2")) == QStringLiteral("=SUM(A1:A2)") &&
                sheet.cells.value(QStringLiteral("A3")) == snapshot.table->cellText(2, 0) &&
                sheet.cells.value(QStringLiteral("B3")) ==
                    QStringLiteral("_x005F_x0041__x000D__x0001_") &&
                sheet.cells.contains(QStringLiteral("C3")) &&
                sheet.cells.value(QStringLiteral("C3")).isEmpty(),
            "literal cells must preserve exact values, whitespace, and SpreadsheetML escapes");
    require(sheet.styles.value(QStringLiteral("A1")) == QStringLiteral("1") &&
                sheet.styles.value(QStringLiteral("A2")) == QStringLiteral("0") &&
                package.value(QStringLiteral("xl/styles.xml")).contains("<b/>") &&
                package.value(QStringLiteral("xl/styles.xml")).contains("wrapText=\"1\""),
            "header cells must be bold and multiline text must wrap");
}

void temporaryWorkbooksOpenOnlyAfterSavingAndSurviveSuccess() {
    auto snapshot = tableSnapshot();
    snapshot.table->setCellText(1, 0, QStringLiteral("雪\n00123"));
    snapshot.table->setCellText(1, 1, QStringLiteral("=SUM(A1:A2)"));
    QStringList paths;
    const auto launch = [&](const QUrl& url) {
        require(url.isLocalFile(), "the default application receives a local file URL");
        const QString path = url.toLocalFile();
        require(QFileInfo(path).fileName().startsWith(QStringLiteral("Table-")) &&
                    QFileInfo(path).suffix() == QStringLiteral("xlsx") &&
                    QFileInfo(QFileInfo(path).absolutePath()).absolutePath() == QDir::tempPath(),
                "workbooks use a reserved directory directly under the system temporary directory");
        const auto sheet = recognition_workbook_test::sheet(recognition_workbook_test::parts(path));
        require(sheet.dimension == QStringLiteral("A1:C3") && sheet.merges.size() == 2 &&
                    sheet.cells.value(QStringLiteral("A2")) == QStringLiteral("雪\n00123") &&
                    sheet.cells.value(QStringLiteral("B2")) == QStringLiteral("=SUM(A1:A2)"),
                "the launched workbook preserves the complete table and literal cell values");
        require(QFile::rename(path, path + QStringLiteral(".probe")) &&
                    QFile::rename(path + QStringLiteral(".probe"), path),
                "the workbook is closed before launching the default application");
        paths.append(path);
        return true;
    };
    require(ScreenshotRecognitionFileExport::openTemporaryTable(snapshot, launch).succeeded() &&
                ScreenshotRecognitionFileExport::openTemporaryTable(snapshot, launch).succeeded() &&
                paths.size() == 2 && paths[0] != paths[1] &&
                QFileInfo(paths[0]).fileName() != QFileInfo(paths[1]).fileName(),
            "repeated opens retain workbooks with unique filenames as well as unique paths");
    for (const QString& path : paths) {
        require(QFileInfo::exists(path),
                "the workbook survives the temporary helper's destruction");
        require(QFile::remove(path) && QDir().rmdir(QFileInfo(path).absolutePath()),
                "remove the test's retained workbook");
    }
    QString rejectedPath;
    const auto rejected =
        ScreenshotRecognitionFileExport::openTemporaryTable(snapshot, [&](const QUrl& url) {
            rejectedPath = url.toLocalFile();
            return false;
        });
    require(!rejected.succeeded() && !rejected.error.isEmpty() && !rejectedPath.isEmpty() &&
                !QFileInfo::exists(QFileInfo(rejectedPath).absolutePath()),
            "rejected launches remove the reserved directory and report an error");
    const auto before =
        QDir(QDir::tempPath())
            .entryList({QStringLiteral("SnowShot-table-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    int launches = 0;
    const auto unexpectedLaunch = [&](const QUrl&) {
        ++launches;
        return true;
    };
    snapshot.table->setCellText(1, 0, QString(32768, QLatin1Char('x')));
    require(!ScreenshotRecognitionFileExport::openTemporaryTable(snapshot, unexpectedLaunch)
                 .succeeded(),
            "workbook serialization failure prevents a launch");
    snapshot.table.reset();
    require(!ScreenshotRecognitionFileExport::openTemporaryTable(snapshot, unexpectedLaunch)
                    .succeeded() &&
                launches == 0 &&
                before == QDir(QDir::tempPath())
                              .entryList({QStringLiteral("SnowShot-table-*")},
                                         QDir::Dirs | QDir::NoDotAndDotDot),
            "missing results and failed exports leave no temporary artifacts");
}

void requireDefaultWorkbookFonts(const QMap<QString, QByteArray>& package) {
    QXmlStreamReader xml(package.value(QStringLiteral("xl/styles.xml")));
    int fonts = 0;
    int boldFonts = 0;
    bool inFont = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.name() == u"font")
            inFont = false;
        if (!xml.isStartElement())
            continue;
        if (xml.name() == u"font") {
            inFont = true;
            ++fonts;
        }
        if (inFont) {
            require(xml.name() != u"name" && xml.name() != u"sz" && xml.name() != u"scheme",
                    "saving column widths must not override the spreadsheet's default font");
            if (xml.name() == u"b")
                ++boldFonts;
        }
    }
    require(!xml.hasError() && fonts == 2 && boldFonts == 1,
            "default workbook fonts must retain normal cells and bold headers");
}

void workbookPreservesColumnWidths() {
    QTemporaryDir directory;
    require(directory.isValid(), "column width export fixture directory");
    auto snapshot = tableSnapshot();
    snapshot.tableColumnWidths = {92, 203, 350};
    const auto verify = [&](const QString& path) {
        requireClassicZipWorkbook(path);
        const auto package = recognition_workbook_test::parts(path);
        requireDefaultWorkbookFonts(package);
        const auto sheet = recognition_workbook_test::sheet(package);
        require(sheet.columnWidths.size() == 3 && sheet.columnWidths.value(1) == 13.140625 &&
                    sheet.columnWidths.value(2) == 29.0 && sheet.columnWidths.value(3) == 50.0,
                "each column must retain its displayed width, including columns in merged cells");
    };
    const auto saved = ScreenshotRecognitionFileExport::saveToPath(
        snapshot, directory.filePath(QStringLiteral("widths.xlsx")));
    require(saved.succeeded(), "save workbook with column widths");
    verify(saved.path);
    const auto quick = ScreenshotRecognitionFileExport::quickSave(snapshot, directory.path(),
                                                                  QStringLiteral("quick-widths"));
    require(quick.succeeded(), "quick save workbook with column widths");
    verify(quick.path);
    const auto opened =
        ScreenshotRecognitionFileExport::openTemporaryTable(snapshot, [&](const QUrl& url) {
            verify(url.toLocalFile());
            return false;
        });
    require(!opened.succeeded(), "declined test launch cleans up the temporary workbook");

    snapshot.tableColumnWidths.clear();
    const auto headless = ScreenshotRecognitionFileExport::saveToPath(
        snapshot, directory.filePath(QStringLiteral("headless.xlsx")));
    require(headless.succeeded() &&
                recognition_workbook_test::sheet(recognition_workbook_test::parts(headless.path))
                    .columnWidths.isEmpty(),
            "headless snapshots without layout keep the workbook's default widths");
    requireDefaultWorkbookFonts(recognition_workbook_test::parts(headless.path));
}

void workbookSaveHandlesCollisionsOverwriteAndFailures() {
    QTemporaryDir directory;
    auto snapshot = tableSnapshot();
    const QString path = directory.filePath(QStringLiteral("Table.xlsx"));
    write(path, QByteArrayLiteral("original"));
    require(!ScreenshotRecognitionFileExport::saveToPath(snapshot, path).succeeded() &&
                read(path) == QByteArrayLiteral("original"),
            "workbook overwrite requires authorization");
    const auto quick = ScreenshotRecognitionFileExport::quickSave(snapshot, directory.path(),
                                                                  QStringLiteral("Table"));
    require(quick.succeeded() && quick.path.endsWith(QStringLiteral("Table_1.xlsx")),
            "quick save must avoid an existing workbook");
    const auto overwritten = ScreenshotRecognitionFileExport::saveToPath(snapshot, path, true);
    require(overwritten.succeeded() && !recognition_workbook_test::parts(path).isEmpty(),
            "authorized overwrite must publish a valid workbook");
    const auto original = read(path);
    snapshot.table->setCellText(1, 0, QString(32768, QLatin1Char('x')));
    require(!ScreenshotRecognitionFileExport::saveToPath(snapshot, path, true).succeeded() &&
                read(path) == original,
            "unsupported cell values must not truncate data or replace an existing workbook");
    snapshot.table.reset();
    require(!snapshot.hasContent() &&
                !ScreenshotRecognitionFileExport::saveToPath(snapshot, path, true).succeeded(),
            "an unavailable table must not produce any file");
    snapshot.table = ScreenshotTableDocument(2, 28);
    snapshot.table->setCellText(1, 27, QStringLiteral("AA boundary"));
    require(ScreenshotRecognitionFileExport::saveToPath(snapshot, path, true).succeeded() &&
                recognition_workbook_test::sheet(recognition_workbook_test::parts(path))
                        .cells.value(QStringLiteral("AB2")) == QStringLiteral("AA boundary"),
            "blank tables and references beyond column Z must remain valid");
    const QString blocked = directory.filePath(QStringLiteral("blocked.xlsx"));
    require(QDir().mkpath(blocked), "create blocked workbook output");
    require(!ScreenshotRecognitionFileExport::saveToPath(snapshot, blocked, true).succeeded() &&
                QFileInfo(blocked).isDir() &&
                QDir(directory.path())
                    .entryList({QStringLiteral(".snow-shot-*")}, QDir::Files | QDir::Hidden)
                    .isEmpty(),
            "failed workbook saves must preserve directories and leave no staging files");
    QTimer::singleShot(0, qApp, [] {
        for (auto* widget : QApplication::topLevelWidgets())
            if (auto* box = qobject_cast<QMessageBox*>(widget))
                box->button(QMessageBox::No)->click();
    });
    require(!ScreenshotRecognitionFileExport::confirmOverwrite(nullptr, {path}),
            "workbook overwrite can be canceled");
}

void savesQrValuesAsOneTextFile() {
    QTemporaryDir directory;
    require(directory.isValid(), "QR save directory unavailable");
    const QString source = QStringList{QStringLiteral("first"), QStringLiteral("雪"),
                                       QStringLiteral("https://example.test")}
                               .join(QLatin1Char('\n'));
    const auto saved = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Qr, source},
        directory.filePath(QStringLiteral("codes.html")));
    require(saved.succeeded() && QFileInfo(saved.path).suffix() == QStringLiteral("txt") &&
                read(saved.path) == source.toUtf8(),
            "QR values must retain their order in one UTF-8 text file");
    require(!QFileInfo::exists(directory.filePath(QStringLiteral("codes.html"))),
            "QR save must not produce an image or conversion file");
}

void savesLatexAsOneTextFile() {
    QTemporaryDir directory;
    require(directory.isValid(), "LaTeX save directory unavailable");
    const QString source = QStringLiteral("\\frac{a_b}{c^2} <x> & y\n+1");
    const auto saved = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Latex, source},
        directory.filePath(QStringLiteral("codes.html")));
    require(saved.succeeded() && QFileInfo(saved.path).suffix() == QStringLiteral("txt") &&
                read(saved.path) == source.toUtf8(),
            "LaTeX source must remain verbatim in one UTF-8 text file");
    require(!QFileInfo::exists(directory.filePath(QStringLiteral("codes.html"))),
            "LaTeX save must not produce an image or conversion file");
}

void quickSaveAvoidsEitherPairCollision() {
    QTemporaryDir directory;
    require(directory.isValid(), "quick save directory unavailable");
    write(directory.filePath(QStringLiteral("Auto.txt")), QByteArrayLiteral("keep"));
    const auto first = ScreenshotRecognitionFileExport::quickSave(
        {ScreenshotRecognitionFileKind::Markdown, QStringLiteral("# partial")}, directory.path(),
        QStringLiteral("Auto"));
    require(first.succeeded() && first.path.endsWith(QStringLiteral("Auto_1.md")) &&
                read(directory.filePath(QStringLiteral("Auto.txt"))) == QByteArrayLiteral("keep") &&
                read(directory.filePath(QStringLiteral("Auto_1.txt"))) ==
                    QByteArrayLiteral("# partial"),
            "a companion collision must move the entire quick-save pair to a new base name");
    const auto second = ScreenshotRecognitionFileExport::quickSave(
        {ScreenshotRecognitionFileKind::Markdown, QStringLiteral("# next")}, directory.path(),
        QStringLiteral("Auto"));
    require(second.succeeded() && second.path.endsWith(QStringLiteral("Auto_2.md")),
            "quick save must preserve existing primary files too");
}

void emptyAndFailedPairLeaveNoPartialOutput() {
    QTemporaryDir directory;
    require(directory.isValid(), "failure save directory unavailable");
    const QString primary = directory.filePath(QStringLiteral("result.html"));
    const QString companion = directory.filePath(QStringLiteral("result.txt"));
    const auto empty = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Html, {}}, primary);
    require(!empty.succeeded() && !QFileInfo::exists(primary),
            "an empty recognition result must not create a file");
    write(primary, QByteArrayLiteral("original"));
    require(QDir().mkpath(companion), "failure companion directory unavailable");
    const auto failed = ScreenshotRecognitionFileExport::saveToPath(
        {ScreenshotRecognitionFileKind::Html, QStringLiteral("replacement")}, primary, true);
    require(!failed.succeeded() && read(primary) == QByteArrayLiteral("original") &&
                QFileInfo(companion).isDir(),
            "a pair publication failure must preserve prior outputs");
}

void longQuickSaveNamesPreservePairsAndCollisions() {
    QTemporaryDir directory;
    require(directory.isValid(), "long recognition filename directory unavailable");
    for (const auto kind :
         {ScreenshotRecognitionFileKind::Markdown, ScreenshotRecognitionFileKind::Html}) {
        const ScreenshotRecognitionFileSnapshot snapshot{kind, QStringLiteral("source")};
        const QString base = QString::fromUtf8("文😀").repeated(100);
        for (int index = 0; index < 2; ++index) {
            const auto result =
                ScreenshotRecognitionFileExport::quickSave(snapshot, directory.path(), base);
            require(result.succeeded(), "long recognition names and their companions must save");
            if (index != 0)
                require(QFileInfo(result.path).completeBaseName().endsWith(QStringLiteral("_1")),
                        "long recognition names must retain the collision index");
            for (const QString& path :
                 ScreenshotRecognitionFileExport::outputPaths(result.path, snapshot)) {
                const QString name = QFileInfo(path).fileName();
                require(read(path) == QByteArrayLiteral("source") && name.toUtf8().size() <= 255 &&
                            QString::fromUtf8(name.toUtf8()) == name,
                        "recognition pairs must fit the component limit and preserve Unicode");
            }
        }
        require(QDir(directory.path()).removeRecursively() && QDir().mkpath(directory.path()),
                "reset the recognition pair fixture");
    }
}

void manualOverwriteConfirmsAndReplacesBothFiles() {
    QTemporaryDir directory;
    require(directory.isValid(), "overwrite directory unavailable");
    const QString primary = directory.filePath(QStringLiteral("result.md"));
    const QString companion = directory.filePath(QStringLiteral("result.txt"));
    write(primary, QByteArrayLiteral("old primary"));
    write(companion, QByteArrayLiteral("old companion"));
    const ScreenshotRecognitionFileSnapshot snapshot{ScreenshotRecognitionFileKind::Markdown,
                                                     QStringLiteral("# new")};
    const QStringList paths = ScreenshotRecognitionFileExport::outputPaths(primary, snapshot);
    const auto confirm = [&](QMessageBox::StandardButton answer) {
        QTimer timer;
        timer.setInterval(10);
        QObject::connect(&timer, &QTimer::timeout, &timer, [answer]() {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                if (auto* box = qobject_cast<QMessageBox*>(widget); box && box->isVisible())
                    box->button(answer)->click();
            }
        });
        timer.start();
        const bool accepted = ScreenshotRecognitionFileExport::confirmOverwrite(nullptr, paths);
        timer.stop();
        return accepted;
    };
    require(!confirm(QMessageBox::No) && read(primary) == QByteArrayLiteral("old primary") &&
                read(companion) == QByteArrayLiteral("old companion"),
            "declining overwrite must preserve both existing files");
    require(confirm(QMessageBox::Yes), "manual overwrite requires explicit confirmation");
    const auto saved = ScreenshotRecognitionFileExport::saveToPath(snapshot, primary, true);
    require(saved.succeeded() && read(primary) == QByteArrayLiteral("# new") &&
                read(companion) == QByteArrayLiteral("# new"),
            "confirmed overwrite must replace both members from one source snapshot");
}

void overwriteButtonsFollowSelectedLanguage() {
    QTemporaryDir directory;
    require(directory.isValid(), "overwrite translation directory unavailable");
    const QString existing = directory.filePath(QStringLiteral("result.txt"));
    write(existing, QByteArrayLiteral("old"));
    struct Case {
        const char* locale;
        const char* replace;
        const char* cancel;
        const char* workbookFilter;
        const char* sheetName;
    };
    for (const Case& test : {Case{"en_US", "Replace", "Cancel", "Excel workbook (*.xlsx)", "Table"},
                             Case{"zh_CN", "替换", "取消", "Excel 工作簿 (*.xlsx)", "表格"},
                             Case{"zh_TW", "取代", "取消", "Excel 活頁簿 (*.xlsx)", "表格"}}) {
        QTranslator translator;
        require(translator.load(
                    QStringLiteral(":/i18n/snow_shot_%1.qm").arg(QString::fromLatin1(test.locale))),
                "overwrite confirmation catalog did not load");
        QCoreApplication::installTranslator(&translator);
        require(ScreenshotRecognitionFileExport::dialogFilter(
                    ScreenshotRecognitionFileKind::Table) == QString::fromUtf8(test.workbookFilter),
                "the Excel file filter must follow the selected language");
        const auto workbook = ScreenshotRecognitionFileExport::saveToPath(
            tableSnapshot(),
            directory.filePath(QString::fromLatin1(test.locale) + QStringLiteral(".xlsx")));
        require(workbook.succeeded(), "localized workbook could not be saved");
        const auto package = recognition_workbook_test::parts(workbook.path);
        QXmlStreamReader xml(package.value(QStringLiteral("xl/workbook.xml")));
        QString sheetName;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == u"sheet")
                sheetName = xml.attributes().value(u"name").toString();
        }
        require(!xml.hasError() && sheetName == QString::fromUtf8(test.sheetName),
                "the worksheet name must follow the selected language");
        bool inspected = false;
        bool labelsMatch = false;
        QTimer::singleShot(0, qApp, [&] {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* dialog = qobject_cast<QMessageBox*>(widget);
                if (dialog == nullptr || !dialog->isVisible())
                    continue;
                inspected = true;
                labelsMatch =
                    dialog->testOption(QMessageBox::Option::DontUseNativeDialog) &&
                    dialog->button(QMessageBox::Yes)->text() == QString::fromUtf8(test.replace) &&
                    dialog->button(QMessageBox::No)->text() == QString::fromUtf8(test.cancel) &&
                    dialog->defaultButton() == dialog->button(QMessageBox::No);
                dialog->button(QMessageBox::No)->click();
                return;
            }
        });
        require(!ScreenshotRecognitionFileExport::confirmOverwrite(nullptr, {existing}),
                "canceling overwrite must decline replacement");
        require(inspected && labelsMatch, "overwrite buttons must use the selected app language");
        QCoreApplication::removeTranslator(&translator);
    }
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    try {
        cancellationLeavesExistingFilesAndCompanionsIntact();
        savesRealWorkbookWithLiteralCellsAndSpans();
        workbookPreservesColumnWidths();
        temporaryWorkbooksOpenOnlyAfterSavingAndSurviveSuccess();
        workbookSaveHandlesCollisionsOverwriteAndFailures();
        savesConversionPairsWithExactSource();
        savesQrValuesAsOneTextFile();
        savesLatexAsOneTextFile();
        quickSaveAvoidsEitherPairCollision();
        longQuickSaveNamesPreservePairsAndCollisions();
        emptyAndFailedPairLeaveNoPartialOutput();
        manualOverwriteConfirmsAndReplacesBothFiles();
        overwriteButtonsFollowSelectedLanguage();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
