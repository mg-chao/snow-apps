#include "snow_shot/presentation/screenshotrecognitionfileexport.h"
#include "snow_shot/storage/exportfilename.h"
#include "snow_shot/presentation/screenshotdocumentexport.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include "snow_shot/storage/clipboardfilecache.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QMimeData>
#include <QTemporaryFile>
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
#include <QDesktopServices>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <QScopeGuard>
#include <QXmlStreamWriter>
#include <mz.h>
#include <mz_strm.h>
#include <mz_strm_mem.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <algorithm>
#include <cmath>
#endif

namespace {
QString message(const char* source) {
    return QCoreApplication::translate("ScreenshotRecognitionFileExport", source);
}

#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
QString cellReference(int row, int column) {
    QString reference;
    for (++column; column > 0; column = (column - 1) / 26)
        reference.prepend(QChar(u'A' + (column - 1) % 26));
    return reference + QString::number(row + 1);
}

QString spreadsheetText(const QString& text) {
    QString result;
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        // SpreadsheetML escapes control characters and literal escape sequences separately.
        const bool escapePrefix =
            character == u'_' && index + 6 < text.size() && text.at(index + 1) == u'x' &&
            text.at(index + 6) == u'_' &&
            std::all_of(text.cbegin() + index + 2, text.cbegin() + index + 6, [](QChar value) {
                return (value >= u'0' && value <= u'9') || (value >= u'a' && value <= u'f') ||
                       (value >= u'A' && value <= u'F');
            });
        if (escapePrefix ||
            (character.unicode() < 0x20 && character != u'\n' && character != u'\t') ||
            character == QChar(0xfffe) || character == QChar(0xffff)) {
            result += QStringLiteral("_x%1_").arg(
                QString::number(static_cast<uint>(character.unicode()), 16)
                    .rightJustified(4, QLatin1Char('0'))
                    .toUpper());
        } else {
            result += character;
        }
    }
    return result;
}

QByteArray tableWorkbook(const ScreenshotTableDocument& document, const QVector<int>& columnWidths,
                         const std::function<bool()>& cancelled) {
    if (document.empty() || document.rowCount() > 1048576 || document.columnCount() > 16384)
        return {};
    void* stream = mz_stream_mem_create();
    if (!stream)
        return {};
    const auto streamGuard = qScopeGuard([&] { mz_stream_mem_delete(&stream); });
    if (mz_stream_mem_open(stream, nullptr, MZ_OPEN_MODE_CREATE | MZ_OPEN_MODE_READWRITE) != MZ_OK)
        return {};
    void* writer = mz_zip_writer_create();
    if (!writer)
        return {};
    const auto writerGuard = qScopeGuard([&] { mz_zip_writer_delete(&writer); });
    if (mz_zip_writer_open(writer, stream, 0) != MZ_OK)
        return {};
    const auto addXml = [writer](const char* name, QStringView root, QStringView nameSpace,
                                 const auto& fill) {
        QByteArray bytes;
        QXmlStreamWriter xml(&bytes);
        xml.writeStartDocument();
        xml.writeStartElement(root);
        xml.writeDefaultNamespace(nameSpace);
        fill(xml);
        xml.writeEndElement();
        xml.writeEndDocument();
        if (xml.hasError())
            return false;
        mz_zip_file entry{};
        entry.filename = name;
        entry.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        // The workbook fits a 32-bit memory stream. Unknown entry sizes make minizip emit
        // ZIP64 local headers even for tiny parts, which older Excel versions reject.
        entry.uncompressed_size = bytes.size();
        entry.zip64 = MZ_ZIP64_DISABLE;
        return mz_zip_writer_add_buffer(writer, bytes.constData(),
                                        static_cast<int32_t>(bytes.size()), &entry) == MZ_OK;
    };
    constexpr auto spreadsheetNamespace =
        u"http://schemas.openxmlformats.org/spreadsheetml/2006/main";
    constexpr auto relationshipNamespace =
        u"http://schemas.openxmlformats.org/package/2006/relationships";
    const auto relationship = [](QXmlStreamWriter& xml, QStringView id, QStringView type,
                                 QStringView target) {
        xml.writeEmptyElement(u"Relationship");
        xml.writeAttribute(u"Id", id);
        xml.writeAttribute(u"Type", QStringLiteral("http://schemas.openxmlformats.org/"
                                                   "officeDocument/2006/relationships/") +
                                        type);
        xml.writeAttribute(u"Target", target);
    };
    if (!addXml(
            "[Content_Types].xml", u"Types",
            u"http://schemas.openxmlformats.org/package/2006/content-types",
            [](QXmlStreamWriter& xml) {
                xml.writeEmptyElement(u"Default");
                xml.writeAttribute(u"Extension", u"rels");
                xml.writeAttribute(u"ContentType",
                                   u"application/vnd.openxmlformats-package.relationships+xml");
                xml.writeEmptyElement(u"Default");
                xml.writeAttribute(u"Extension", u"xml");
                xml.writeAttribute(u"ContentType", u"application/xml");
                const auto part = [&](QStringView name, QStringView type) {
                    xml.writeEmptyElement(u"Override");
                    xml.writeAttribute(u"PartName", name);
                    xml.writeAttribute(u"ContentType", type);
                };
                part(u"/xl/workbook.xml",
                     u"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml");
                part(u"/xl/worksheets/sheet1.xml",
                     u"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml");
                part(u"/xl/styles.xml",
                     u"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml");
            }) ||
        !addXml("_rels/.rels", u"Relationships", relationshipNamespace,
                [&](QXmlStreamWriter& xml) {
                    relationship(xml, u"rId1", u"officeDocument", u"xl/workbook.xml");
                }) ||
        !addXml("xl/workbook.xml", u"workbook", spreadsheetNamespace,
                [](QXmlStreamWriter& xml) {
                    xml.writeNamespace(
                        u"http://schemas.openxmlformats.org/officeDocument/2006/relationships",
                        u"r");
                    xml.writeStartElement(u"sheets");
                    xml.writeEmptyElement(u"sheet");
                    xml.writeAttribute(u"name", message(QT_TRANSLATE_NOOP(
                                                    "ScreenshotRecognitionFileExport", "Table")));
                    xml.writeAttribute(u"sheetId", u"1");
                    xml.writeAttribute(u"r:id", u"rId1");
                    xml.writeEndElement();
                }) ||
        !addXml("xl/_rels/workbook.xml.rels", u"Relationships", relationshipNamespace,
                [&](QXmlStreamWriter& xml) {
                    relationship(xml, u"rId1", u"worksheet", u"worksheets/sheet1.xml");
                    relationship(xml, u"rId2", u"styles", u"styles.xml");
                }) ||
        !addXml("xl/styles.xml", u"styleSheet", spreadsheetNamespace, [](QXmlStreamWriter& xml) {
            xml.writeStartElement(u"fonts");
            xml.writeAttribute(u"count", u"2");
            // Preserve viewer-selected fonts; column sizing must not change cell typography.
            xml.writeEmptyElement(u"font");
            xml.writeStartElement(u"font");
            xml.writeEmptyElement(u"b");
            xml.writeEndElement();
            xml.writeEndElement();
            xml.writeStartElement(u"fills");
            xml.writeAttribute(u"count", u"2");
            for (const auto pattern : {u"none", u"gray125"}) {
                xml.writeStartElement(u"fill");
                xml.writeEmptyElement(u"patternFill");
                xml.writeAttribute(u"patternType", pattern);
                xml.writeEndElement();
            }
            xml.writeEndElement();
            xml.writeStartElement(u"borders");
            xml.writeAttribute(u"count", u"1");
            xml.writeEmptyElement(u"border");
            xml.writeEndElement();
            for (const auto group : {u"cellStyleXfs", u"cellXfs"}) {
                const bool cells = QStringView(group) == u"cellXfs";
                xml.writeStartElement(group);
                xml.writeAttribute(u"count", cells ? u"2" : u"1");
                for (int index = 0; index < (cells ? 2 : 1); ++index) {
                    xml.writeStartElement(u"xf");
                    xml.writeAttribute(u"numFmtId", u"49");
                    xml.writeAttribute(u"fontId", QString::number(index));
                    xml.writeAttribute(u"fillId", u"0");
                    xml.writeAttribute(u"borderId", u"0");
                    if (cells) {
                        xml.writeAttribute(u"xfId", u"0");
                        xml.writeAttribute(u"applyNumberFormat", u"1");
                        xml.writeAttribute(u"applyFont", u"1");
                        xml.writeAttribute(u"applyAlignment", u"1");
                        xml.writeEmptyElement(u"alignment");
                        xml.writeAttribute(u"wrapText", u"1");
                        xml.writeAttribute(u"vertical", u"top");
                    }
                    xml.writeEndElement();
                }
                xml.writeEndElement();
            }
            xml.writeStartElement(u"cellStyles");
            xml.writeAttribute(u"count", u"1");
            xml.writeEmptyElement(u"cellStyle");
            xml.writeAttribute(u"name", u"Normal");
            xml.writeAttribute(u"xfId", u"0");
            xml.writeAttribute(u"builtinId", u"0");
            xml.writeEndElement();
        }))
        return {};
    bool validCells = true;
    if (!addXml("xl/worksheets/sheet1.xml", u"worksheet", spreadsheetNamespace,
                [&](QXmlStreamWriter& xml) {
                    xml.writeEmptyElement(u"dimension");
                    xml.writeAttribute(u"ref", QStringLiteral("A1:") +
                                                   cellReference(document.rowCount() - 1,
                                                                 document.columnCount() - 1));
                    if (!columnWidths.isEmpty()) {
                        xml.writeStartElement(u"cols");
                        for (int column = 0;
                             column < document.columnCount() && column < columnWidths.size();
                             ++column) {
                            const int pixels = columnWidths.at(column);
                            if (pixels <= 0)
                                continue;
                            // OOXML stores the padded width in 1/256 digit-width units.
                            // QTableView widths already include padding; use logical pixels,
                            // without multiplying by the screen's device pixel ratio.
                            // Use a 7-pixel reference digit width without forcing a cell font.
                            // The viewer's default font can affect the final rendered width.
                            const double width =
                                std::min(255.0, std::floor(pixels * 256.0 / 7.0) / 256.0);
                            xml.writeEmptyElement(u"col");
                            xml.writeAttribute(u"min", QString::number(column + 1));
                            xml.writeAttribute(u"max", QString::number(column + 1));
                            xml.writeAttribute(u"width", QString::number(width, 'f', 8));
                            xml.writeAttribute(u"customWidth", u"1");
                        }
                        xml.writeEndElement();
                    }
                    xml.writeStartElement(u"sheetData");
                    QStringList merges;
                    for (int row = 0; row < document.rowCount(); ++row) {
                        if (cancelled && cancelled()) {
                            validCells = false;
                            return;
                        }
                        xml.writeStartElement(u"row");
                        xml.writeAttribute(u"r", QString::number(row + 1));
                        for (int column = 0; column < document.columnCount(); ++column) {
                            const auto* cell = document.anchorCellAt(row, column);
                            if (!cell)
                                continue;
                            if (cell->text.size() > 32767) {
                                validCells = false;
                                return;
                            }
                            const QString reference = cellReference(row, column);
                            xml.writeStartElement(u"c");
                            xml.writeAttribute(u"r", reference);
                            xml.writeAttribute(u"t", u"inlineStr");
                            xml.writeAttribute(u"s", cell->header ? u"1" : u"0");
                            xml.writeStartElement(u"is");
                            xml.writeStartElement(u"t");
                            xml.writeAttribute(u"xml:space", u"preserve");
                            xml.writeCharacters(spreadsheetText(cell->text));
                            xml.writeEndElement();
                            xml.writeEndElement();
                            xml.writeEndElement();
                            if (cell->rowSpan > 1 || cell->columnSpan > 1)
                                merges.append(reference + u':' +
                                              cellReference(row + cell->rowSpan - 1,
                                                            column + cell->columnSpan - 1));
                        }
                        xml.writeEndElement();
                    }
                    xml.writeEndElement();
                    if (!merges.isEmpty()) {
                        xml.writeStartElement(u"mergeCells");
                        xml.writeAttribute(u"count", QString::number(merges.size()));
                        for (const auto& range : merges) {
                            xml.writeEmptyElement(u"mergeCell");
                            xml.writeAttribute(u"ref", range);
                        }
                        xml.writeEndElement();
                    }
                }) ||
        !validCells || mz_zip_writer_close(writer) != MZ_OK)
        return {};
    const void* buffer = nullptr;
    int32_t length = 0;
    if (mz_stream_mem_get_buffer(stream, &buffer) != MZ_OK)
        return {};
    mz_stream_mem_get_buffer_length(stream, &length);
    return QByteArray(static_cast<const char*>(buffer), length);
}
#endif

QString reserveTemporaryPath(const QString& directory) {
    QTemporaryFile temporary(QDir(directory).filePath(QStringLiteral(".snow-shot-XXXXXX")));
    temporary.setAutoRemove(false);
    if (!temporary.open())
        return {};
    const QString path = temporary.fileName();
    temporary.close();
    return path;
}

QStringList existingPaths(const QStringList& paths) {
    QStringList existing;
    for (const QString& path : paths) {
        if (QFileInfo::exists(path))
            existing.push_back(path);
    }
    return existing;
}
} // namespace

QString ScreenshotRecognitionFileExport::pathForSelectedFilter(const QString& path,
                                                               const QString& filter) {
    for (const auto* extension : {"docx", "pdf", "md", "tex"}) {
        const auto suffix = QString::fromLatin1(extension);
        if (filter.contains(QStringLiteral("(*.%1)").arg(suffix))) {
            const auto info = QFileInfo(path);
            return info.dir().filePath(info.completeBaseName() + u'.' + suffix);
        }
    }
    return path;
}

ScreenshotRecognitionFileSaveResult
ScreenshotRecognitionFileExport::copyDocumentFile(const ScreenshotRecognitionFileSnapshot& snapshot,
                                                  const std::function<bool()>& cancelled) {
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    QString error;
    auto directory = snow_shot::storage::ClipboardFileCache::shared().createDirectory(&error);
    if (!directory)
        return {{}, error};
    auto html = snapshot;
    html.kind = ScreenshotRecognitionFileKind::DocumentHtml;
    const auto result = saveToPath(
        html, QDir(directory->path()).filePath(QStringLiteral("Document.html")), false, cancelled);
    if (result.succeeded())
        directory->retain();
    return result;
#else
    Q_UNUSED(snapshot)
    Q_UNUSED(cancelled)
    return {};
#endif
}

#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
namespace {
struct PreparedDocumentCopy {
    ScreenshotDocumentClipboard content;
    QString path;
    QString error;

    explicit PreparedDocumentCopy(const ScreenshotDocumentCopyRequest& request,
                                  const std::function<bool()>& cancelled = {}) {
        if (request.asFile) {
            ScreenshotRecognitionFileSnapshot snapshot;
            snapshot.kind = ScreenshotRecognitionFileKind::DocumentHtml;
            snapshot.document = request.document;
            auto saved = ScreenshotRecognitionFileExport::copyDocumentFile(snapshot, cancelled);
            path = std::move(saved.path);
            error = std::move(saved.error);
        } else if (request.selectionHtml.isEmpty()) {
            content = request.document->clipboardContent(cancelled);
        } else {
            auto selection = *request.document;
            selection.preparedClipboard.reset();
            selection.html = request.selectionHtml;
            content = selection.clipboardContent(cancelled);
        }
    }

    std::unique_ptr<QMimeData> mimeData() const {
        if (path.isEmpty() && content.html.isEmpty())
            return {};
        auto mime = std::make_unique<QMimeData>();
        if (path.isEmpty()) {
            mime->setHtml(content.html);
            mime->setText(content.text);
        } else {
            mime->setUrls({QUrl::fromLocalFile(path)});
        }
        return mime;
    }
};
} // namespace

std::unique_ptr<QMimeData> ScreenshotDocumentCopyRequest::mimeData() const {
    return document ? PreparedDocumentCopy(*this).mimeData() : nullptr;
}

ScreenshotExportJobHandle ScreenshotDocumentCopyRequest::start(QObject* receiver,
                                                               Completion completion) const {
    if (!document)
        return {};
    auto prepared = std::make_shared<std::optional<PreparedDocumentCopy>>();
    return ScreenshotExportCoordinator::shared().submit(
        receiver, ScreenshotExportCoordinator::Priority::Foreground,
        [request = *this, prepared](const ScreenshotExportCancellation& cancellation) {
            prepared->emplace(request, [&] { return cancellation.isCancellationRequested(); });
            if (cancellation.isCancellationRequested())
                return ScreenshotExportTaskResult::failure(ScreenshotExportFailureStage::Cancelled,
                                                           {});
            return ScreenshotExportTaskResult{};
        },
        [prepared, completion = std::move(completion)](const ScreenshotExportTaskResult& result) {
            if (result.failureStage == ScreenshotExportFailureStage::Cancelled)
                return;
            completion(*prepared ? (*prepared)->mimeData() : nullptr,
                       *prepared ? (*prepared)->error : result.error);
        },
        document->retainedBytes() * 2);
}
#endif

QString ScreenshotRecognitionFileExport::extension(ScreenshotRecognitionFileKind kind) {
    switch (kind) {
    case ScreenshotRecognitionFileKind::DocumentWord:
        return QStringLiteral("docx");
    case ScreenshotRecognitionFileKind::DocumentPdf:
        return QStringLiteral("pdf");
    case ScreenshotRecognitionFileKind::DocumentMarkdown:
        return QStringLiteral("md");
    case ScreenshotRecognitionFileKind::DocumentLatex:
        return QStringLiteral("tex");
    case ScreenshotRecognitionFileKind::DocumentHtml:
        return QStringLiteral("html");
    case ScreenshotRecognitionFileKind::Html:
        return QStringLiteral("html");
    case ScreenshotRecognitionFileKind::Markdown:
        return QStringLiteral("md");
    case ScreenshotRecognitionFileKind::Table:
        return QStringLiteral("xlsx");
    case ScreenshotRecognitionFileKind::Latex:
    case ScreenshotRecognitionFileKind::Qr:
        return QStringLiteral("txt");
    }
    return {};
}

QString ScreenshotRecognitionFileExport::dialogFilter(ScreenshotRecognitionFileKind kind) {
    if (kind >= ScreenshotRecognitionFileKind::DocumentWord)
        return message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                         "Word document (*.docx);;PDF document (*.pdf);;Markdown "
                                         "document (*.md);;LaTeX document (*.tex)"));
    switch (kind) {
    case ScreenshotRecognitionFileKind::Html:
        return message(
            QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "HTML document (*.html)"));
    case ScreenshotRecognitionFileKind::Markdown:
        return message(
            QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Markdown document (*.md)"));
    case ScreenshotRecognitionFileKind::Table:
        return message(
            QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Excel workbook (*.xlsx)"));
    case ScreenshotRecognitionFileKind::DocumentWord:
    case ScreenshotRecognitionFileKind::DocumentPdf:
    case ScreenshotRecognitionFileKind::DocumentMarkdown:
    case ScreenshotRecognitionFileKind::DocumentLatex:
    case ScreenshotRecognitionFileKind::DocumentHtml:
    case ScreenshotRecognitionFileKind::Latex:
    case ScreenshotRecognitionFileKind::Qr:
        return message(
            QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Text document (*.txt)"));
    }
    return {};
}

QString ScreenshotRecognitionFileExport::normalizedPath(const QString& path,
                                                        ScreenshotRecognitionFileKind kind) {
    QString normalized = QDir::cleanPath(path.trimmed());
    if (path.trimmed().isEmpty())
        return {};
    const QString fileName = QFileInfo(normalized).fileName();
    const qsizetype dot = fileName.lastIndexOf(QLatin1Char('.'));
    if (dot > 0 && dot + 1 < fileName.size())
        normalized.chop(fileName.size() - dot);
    else if (normalized.endsWith(QLatin1Char('.')))
        normalized.chop(1);
    return normalized + QLatin1Char('.') + extension(kindForPath(kind, path));
}

QStringList
ScreenshotRecognitionFileExport::outputPaths(const QString& primaryPath,
                                             const ScreenshotRecognitionFileSnapshot& snapshot) {
    if (primaryPath.isEmpty())
        return {};
    const auto kind = kindForPath(snapshot.kind, primaryPath);
    QStringList paths{primaryPath};
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    if (kind == ScreenshotRecognitionFileKind::DocumentLatex ||
        (kind == ScreenshotRecognitionFileKind::DocumentMarkdown && snapshot.document &&
         !snapshot.document->markdownImageIds().isEmpty()))
        paths.push_back(
            QFileInfo(primaryPath).dir().filePath(screenshotDocumentImageDirectoryName(primaryPath)));
#endif
    if (kind == ScreenshotRecognitionFileKind::Html ||
        kind == ScreenshotRecognitionFileKind::Markdown) {
        QString companion = primaryPath;
        companion.chop(extension(kind).size());
        companion += QStringLiteral("txt");
        paths.push_back(companion);
    }
    return paths;
}

bool ScreenshotRecognitionFileExport::confirmOverwrite(QWidget* owner, const QStringList& paths) {
    const QStringList existing = existingPaths(paths);
    if (existing.isEmpty())
        return true;
    QMessageBox dialog(
        QMessageBox::Question,
        message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Replace existing files?")),
        message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                  "Replace the existing file(s)?\n%1"))
            .arg(existing.join(QLatin1Char('\n'))),
        QMessageBox::Yes | QMessageBox::No, owner);
    dialog.setOption(QMessageBox::Option::DontUseNativeDialog);
    dialog.button(QMessageBox::Yes)
        ->setText(message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Replace")));
    dialog.button(QMessageBox::No)
        ->setText(message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Cancel")));
    dialog.setDefaultButton(QMessageBox::No);
    return dialog.exec() == QMessageBox::Yes;
}

#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
ScreenshotRecognitionFileSaveResult ScreenshotRecognitionFileExport::openTemporaryTable(
    const ScreenshotRecognitionFileSnapshot& snapshot, std::function<bool(const QUrl&)> openUrl) {
    if (snapshot.kind != ScreenshotRecognitionFileKind::Table || !snapshot.hasContent())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "No recognition result is available to save"))};
    QTemporaryDir directory(
        QDir(QDir::tempPath()).filePath(QStringLiteral("SnowShot-table-XXXXXX")));
    if (!directory.isValid())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The temporary directory could not be created"))};
    const QString fileName =
        QStringLiteral("Table-%1.xlsx").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const auto result = saveToPath(snapshot, directory.filePath(fileName));
    if (!result.succeeded())
        return result;
    const QUrl url = QUrl::fromLocalFile(result.path);
    if (!(openUrl ? openUrl(url) : QDesktopServices::openUrl(url)))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The spreadsheet file could not be opened"))};
    directory.setAutoRemove(false);
    return result;
}
#endif

ScreenshotRecognitionFileKind
ScreenshotRecognitionFileExport::kindForPath(ScreenshotRecognitionFileKind kind,
                                             const QString& path) {
    if (kind < ScreenshotRecognitionFileKind::DocumentWord)
        return kind;
    const auto suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QStringLiteral("docx"))
        return ScreenshotRecognitionFileKind::DocumentWord;
    if (suffix == QStringLiteral("pdf"))
        return ScreenshotRecognitionFileKind::DocumentPdf;
    if (suffix == QStringLiteral("md"))
        return ScreenshotRecognitionFileKind::DocumentMarkdown;
    if (suffix == QStringLiteral("tex"))
        return ScreenshotRecognitionFileKind::DocumentLatex;
    if (suffix == QStringLiteral("html"))
        return ScreenshotRecognitionFileKind::DocumentHtml;
    return kind;
}

#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
ScreenshotRecognitionFileSaveResult ScreenshotRecognitionFileExport::openTemporaryDocument(
    const ScreenshotRecognitionFileSnapshot& snapshot, std::function<bool(const QUrl&)> openUrl) {
    QTemporaryDir directory(QDir::tempPath() + QStringLiteral("/SnowShot-document-XXXXXX"));
    if (!directory.isValid())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The temporary directory could not be created"))};
    auto word = snapshot;
    word.kind = ScreenshotRecognitionFileKind::DocumentWord;
    const auto result = saveToPath(word, directory.filePath(QStringLiteral("Document.docx")));
    if (!result.succeeded())
        return result;
    const auto url = QUrl::fromLocalFile(result.path);
    if (!(openUrl ? openUrl(url) : QDesktopServices::openUrl(url)))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The document file could not be opened"))};
    directory.setAutoRemove(false);
    return result;
}

namespace {
ScreenshotRecognitionFileSaveResult saveDocument(const ScreenshotRecognitionFileSnapshot& snapshot,
                                                 const QString& primary, bool overwrite,
                                                 const std::function<bool()>& cancelled) {
    const auto kind = ScreenshotRecognitionFileExport::kindForPath(snapshot.kind, primary);
    const QFileInfo info(primary);
    QDir parent = info.dir();
    if (!parent.mkpath(QStringLiteral(".")))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "Cannot create output directory"))};
    const auto outputs = ScreenshotRecognitionFileExport::outputPaths(primary, snapshot);
    for (qsizetype index = 0; index < outputs.size(); ++index) {
        const QFileInfo target(outputs[index]);
        if (target.isSymLink() ||
            (target.exists() && (index == 0 ? !target.isFile() : !target.isDir())))
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An existing output file could not be replaced"))};
    }
    for (const auto& output : outputs)
        if (QFileInfo::exists(output) && !overwrite)
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An output file already exists"))};
    QTemporaryDir staging(parent.filePath(QStringLiteral(".snow-document-XXXXXX")));
    if (!staging.isValid())
        return {
            {},
            message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport", "Cannot stage document"))};
    const auto files =
        screenshotDocumentFiles(*snapshot.document, info.fileName(),
                                ScreenshotRecognitionFileExport::extension(kind), cancelled);
    if (cancelled && cancelled())
        return {{}, {}, true};
    if (files.isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The document file could not be created"))};
    for (auto it = files.cbegin(); it != files.cend(); ++it) {
        const QString staged = staging.filePath(it.key());
        if (!QDir().mkpath(QFileInfo(staged).absolutePath()))
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "Cannot stage document images"))};
        QFile file(staged);
        if (!file.open(QIODevice::WriteOnly) || file.write(it.value()) != it.value().size() ||
            !file.flush())
            return {{}, file.errorString()};
    }
    // LaTeX retains its image directory even when the document has no images.
    if (kind == ScreenshotRecognitionFileKind::DocumentLatex)
        QDir().mkpath(staging.filePath(QFileInfo(outputs[1]).fileName()));
    if (cancelled && cancelled())
        return {{}, {}, true};
    QStringList backups, published;
    const auto rollback = [&] {
        for (const auto& target : published)
            QDir().rename(target, staging.filePath(QStringLiteral("rollback-") +
                                                   QFileInfo(target).fileName()));
        for (qsizetype i = 0; i < backups.size(); ++i)
            if (!backups[i].isEmpty())
                QDir().rename(backups[i], outputs[i]);
    };
    for (const auto& target : outputs) {
        if (cancelled && cancelled()) {
            rollback();
            return {{}, {}, true};
        }
        QString backup;
        if (QFileInfo::exists(target)) {
            backup = staging.filePath(QStringLiteral("backup-") + QFileInfo(target).fileName());
            if (!overwrite || !QDir().rename(target, backup)) {
                rollback();
                return {{},
                        message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                                  "Cannot replace document output"))};
            }
        }
        backups.push_back(backup);
        if (!QDir().rename(staging.filePath(QFileInfo(target).fileName()), target)) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "Cannot publish document output"))};
        }
        published.push_back(target);
    }
    return {primary, {}};
}
} // namespace
#endif

ScreenshotRecognitionFileSaveResult
ScreenshotRecognitionFileExport::saveToPath(const ScreenshotRecognitionFileSnapshot& snapshot,
                                            const QString& path, bool allowOverwrite,
                                            std::function<bool()> cancelled) {
    if (cancelled && cancelled())
        return {{}, {}, true};
    if (!snapshot.hasContent())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "No recognition result is available to save"))};
    const QString primary = normalizedPath(path, snapshot.kind);
    if (primary.isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "No output file was selected"))};
#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
    if (snapshot.kind >= ScreenshotRecognitionFileKind::DocumentWord)
        return saveDocument(snapshot, primary, allowOverwrite, cancelled);
#endif
    const QStringList paths = outputPaths(primary, snapshot);
    const QString directory = QFileInfo(primary).absolutePath();
    if (!QDir().mkpath(directory))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The output directory could not be created"))};
    for (const QString& output : paths) {
        const QFileInfo information(output);
        if (information.exists() && !information.isFile())
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An existing output file could not be replaced"))};
    }
    if (!allowOverwrite && !existingPaths(paths).isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "An output file already exists"))};

    QByteArray bytes;
#if SNOW_SHOT_ENABLE_TABLE_RECOGNITION
    if (snapshot.kind == ScreenshotRecognitionFileKind::Table) {
        bytes = tableWorkbook(*snapshot.table, snapshot.tableColumnWidths, cancelled);
        if (cancelled && cancelled())
            return {{}, {}, true};
        if (bytes.isEmpty())
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "The Excel workbook could not be created"))};
    } else
#endif
        bytes = snapshot.source.toUtf8();
    if (cancelled && cancelled())
        return {{}, {}, true};
    QStringList staged;
    QStringList backups(paths.size());
    QStringList published;
    auto rollback = [&]() {
        for (const QString& publishedPath : published)
            QFile::remove(publishedPath);
        for (qsizetype index = 0; index < backups.size(); ++index) {
            if (!backups.at(index).isEmpty())
                QFile::rename(backups.at(index), paths.at(index));
        }
        for (const QString& stagedPath : staged)
            QFile::remove(stagedPath);
    };
    for (qsizetype index = 0; index < paths.size(); ++index) {
        if (cancelled && cancelled()) {
            rollback();
            return {{}, {}, true};
        }
        const QString temporary = reserveTemporaryPath(directory);
        if (temporary.isEmpty()) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "The result file could not be staged"))};
        }
        staged.push_back(temporary);
        QFile file(temporary);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(bytes) != bytes.size() || !file.flush()) {
            const QString error = file.errorString();
            file.close();
            rollback();
            return {{}, error};
        }
        file.close();
    }
    // Publication is one short transaction. Cancel before backups, then finish or roll back
    // the complete primary/companion pair without interrupting the rename sequence.
    if (cancelled && cancelled()) {
        rollback();
        return {{}, {}, true};
    }
    if (!allowOverwrite && !existingPaths(paths).isEmpty()) {
        rollback();
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "An output file already exists"))};
    }
    for (qsizetype index = 0; index < paths.size(); ++index) {
        if (!QFileInfo::exists(paths.at(index)))
            continue;
        if (!allowOverwrite) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An output file already exists"))};
        }
        if (!QFileInfo(paths.at(index)).isFile()) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An existing output file could not be replaced"))};
        }
        const QString backup = reserveTemporaryPath(directory);
        if (backup.isEmpty() || !QFile::remove(backup) || !QFile::rename(paths.at(index), backup)) {
            if (!backup.isEmpty())
                QFile::remove(backup);
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "An existing output file could not be replaced"))};
        }
        backups[index] = backup;
    }
    for (qsizetype index = 0; index < paths.size(); ++index) {
        if (!QFile::rename(staged.at(index), paths.at(index))) {
            rollback();
            return {{},
                    message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                              "The result file could not be published"))};
        }
        published.push_back(paths.at(index));
    }
    for (const QString& backup : backups) {
        if (!backup.isEmpty())
            QFile::remove(backup);
    }
    return {primary, {}};
}

ScreenshotRecognitionFileSaveResult
ScreenshotRecognitionFileExport::quickSave(const ScreenshotRecognitionFileSnapshot& snapshot,
                                           const QString& directory, const QString& baseName,
                                           std::function<bool()> cancelled) {
    if (cancelled && cancelled())
        return {{}, {}, true};
    if (!snapshot.hasContent())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "No recognition result is available to save"))};
    if (directory.trimmed().isEmpty())
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The save directory is not configured"))};
    if (baseName.isEmpty() || baseName.contains(QLatin1Char('/')) ||
        baseName.contains(QLatin1Char('\\')))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The screenshot filename format is invalid"))};
    const QString target = QDir::cleanPath(directory.trimmed());
    if (!QDir().mkpath(target))
        return {{},
                message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                          "The output directory could not be created"))};
    for (int suffix = 0; suffix < 1000000; ++suffix) {
        if (cancelled && cancelled())
            return {{}, {}, true};
        const QString ending = (suffix == 0 ? QString() : QStringLiteral("_%1").arg(suffix)) +
                               QLatin1Char('.') + extension(snapshot.kind);
        // A Markdown companion has a longer extension than its primary file. Reserve enough
        // space for every output so the pair always shares the same bounded stem.
        QString longestEnding = ending;
        for (const QString& output : outputPaths(ending, snapshot)) {
            const auto fileName = QFileInfo(output).fileName();
            if (fileName.toUtf8().size() > longestEnding.toUtf8().size())
                longestEnding = fileName;
        }
        QString stem = snow_shot::storage::composeExportFileName(baseName, longestEnding);
        stem.chop(longestEnding.size());
        const QString path = QDir(target).filePath(stem + ending);
        if (existingPaths(outputPaths(path, snapshot)).isEmpty()) {
            const auto result = saveToPath(snapshot, path, false, cancelled);
            if (result.error != message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                                          "An output file already exists")))
                return result;
        }
    }
    return {{},
            message(QT_TRANSLATE_NOOP("ScreenshotRecognitionFileExport",
                                      "No available output filename was found"))};
}
