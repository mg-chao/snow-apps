#include "snow_shot/presentation/screenshotdocumentexport.h"
#include <QBuffer>
#include <QAbstractTextDocumentLayout>
#include <QFileInfo>
#include <QPageLayout>
#include <QPdfWriter>
#include <QScopeGuard>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextTable>
#include <mz.h>
#include <mz_strm.h>
#include <mz_strm_mem.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <algorithm>

#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
namespace {
QByteArray png(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return {};
    return bytes;
}
QString xmlText(QString text) {
    for (qsizetype i = text.size(); i > 0; --i) {
        const auto c = text[i - 1].unicode();
        if ((c < 32 && c != 9 && c != 10 && c != 13) || c == 0xfffe || c == 0xffff)
            text.remove(i - 1, 1);
    }
    return text.toHtmlEscaped();
}
QByteArray zip(const QMap<QString, QByteArray>& entries, const std::function<bool()>& cancelled) {
    void* stream = mz_stream_mem_create();
    if (!stream)
        return {};
    const auto sg = qScopeGuard([&] { mz_stream_mem_delete(&stream); });
    if (mz_stream_mem_open(stream, nullptr, MZ_OPEN_MODE_CREATE | MZ_OPEN_MODE_READWRITE) != MZ_OK)
        return {};
    void* writer = mz_zip_writer_create();
    if (!writer)
        return {};
    const auto wg = qScopeGuard([&] { mz_zip_writer_delete(&writer); });
    if (mz_zip_writer_open(writer, stream, 0) != MZ_OK)
        return {};
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        if (cancelled && cancelled())
            return {};
        const QByteArray name = it.key().toUtf8();
        mz_zip_file entry{};
        entry.filename = name.constData();
        entry.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        entry.uncompressed_size = it.value().size();
        entry.zip64 = MZ_ZIP64_DISABLE;
        if (mz_zip_writer_add_buffer(writer, it.value().constData(),
                                     static_cast<int32_t>(it.value().size()), &entry) != MZ_OK)
            return {};
    }
    if (mz_zip_writer_close(writer) != MZ_OK)
        return {};
    const void* bytes = nullptr;
    int32_t length = 0;
    if (mz_stream_mem_get_buffer(stream, &bytes) != MZ_OK)
        return {};
    mz_stream_mem_get_buffer_length(stream, &length);
    return QByteArray(static_cast<const char*>(bytes), length);
}

class WordWriter {
  public:
    explicit WordWriter(const ScreenshotRecognizedDocument& value) : source(value) {}
    QByteArray write(const std::function<bool()>& cancelled) {
        auto keys = source.images.keys();
        std::sort(keys.begin(), keys.end());
        for (const auto& key : keys) {
            const int id = static_cast<int>(assets.size()) + 1;
            assets.insert(key, id);
            const QString path = QStringLiteral("media/image%1.png").arg(id);
            const auto bytes = png(source.images.value(key));
            if (bytes.isEmpty())
                return {};
            parts.insert(QStringLiteral("word/") + path, bytes);
            rels += QStringLiteral("<Relationship Id=\"image%1\" Type=\"%2image\" Target=\"%3\"/>")
                        .arg(id)
                        .arg(office, path);
        }
        auto document = source.createTextDocument();
        const QString body = frame(document->rootFrame(), cancelled);
        if (cancelled && cancelled())
            return {};
        parts.insert(
            QStringLiteral("word/document.xml"),
            (QStringLiteral(
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?><w:document "
                 "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
                 "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                 "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/"
                 "wordprocessingDrawing\" "
                 "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                 "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/"
                 "picture\"><w:body>") +
             body +
             QStringLiteral(
                 "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
                 "<w:pgMar w:top=\"1134\" w:right=\"1134\" w:bottom=\"1134\" w:left=\"1134\"/>"
                 "</w:sectPr></w:body></w:document>"))
                .toUtf8());
        if (!source.formulas.isEmpty()) {
            QString sources = QStringLiteral("<formulas xmlns=\"urn:snow-shot:document-math\">");
            for (const auto& formula : source.formulas)
                sources +=
                    QStringLiteral("<formula id=\"%1\" display=\"%2\">%3</formula>")
                        .arg(xmlText(formula.id),
                             formula.display ? QStringLiteral("true") : QStringLiteral("false"),
                             xmlText(formula.source));
            parts.insert(QStringLiteral("customXml/item1.xml"),
                         (sources + QStringLiteral("</formulas>")).toUtf8());
            rels += QStringLiteral("<Relationship Id=\"formulaSources\" Type=\"%1customXml\" "
                                   "Target=\"../customXml/item1.xml\"/>")
                        .arg(office);
        }
        parts.insert(QStringLiteral("word/_rels/document.xml.rels"),
                     (rels + QStringLiteral("</Relationships>")).toUtf8());
        parts.insert(
            QStringLiteral("_rels/.rels"),
            QStringLiteral("<Relationships "
                           "xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                           "<Relationship Id=\"document\" Type=\"%1officeDocument\" "
                           "Target=\"word/document.xml\"/>"
                           "</Relationships>")
                .arg(office)
                .toUtf8());
        parts.insert(
            QStringLiteral("[Content_Types].xml"),
            QByteArray(
                "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                "<Default Extension=\"rels\" "
                "ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                "<Default Extension=\"png\" ContentType=\"image/png\"/>"
                "<Override PartName=\"/word/document.xml\" "
                "ContentType=\"application/"
                "vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
                "<Override PartName=\"/word/styles.xml\" "
                "ContentType=\"application/"
                "vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
                "<Override PartName=\"/word/numbering.xml\" "
                "ContentType=\"application/"
                "vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/></Types>"));
        QString styles = QStringLiteral(
            "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
            "<w:docDefaults><w:rPrDefault><w:rPr><w:sz "
            "w:val=\"22\"/></w:rPr></w:rPrDefault></w:docDefaults>"
            "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name "
            "w:val=\"Normal\"/></w:style>");
        for (int level = 1; level <= 6; ++level)
            styles +=
                QStringLiteral("<w:style w:type=\"paragraph\" w:styleId=\"Heading%1\">"
                               "<w:name w:val=\"heading %1\"/><w:basedOn w:val=\"Normal\"/><w:pPr>"
                               "<w:keepNext/><w:outlineLvl w:val=\"%2\"/></w:pPr><w:rPr><w:b/>"
                               "<w:sz w:val=\"%3\"/></w:rPr></w:style>")
                    .arg(level)
                    .arg(level - 1)
                    .arg(36 - level * 2);
        parts.insert(QStringLiteral("word/styles.xml"),
                     (styles + QStringLiteral("</w:styles>")).toUtf8());
        parts.insert(QStringLiteral("word/numbering.xml"),
                     (numbering + numberingInstances + QStringLiteral("</w:numbering>")).toUtf8());
        return zip(parts, cancelled);
    }

  private:
    QString paragraph(const QTextBlock& block) {
        QString out = QStringLiteral("<w:p><w:pPr>");
        if (block.blockFormat().headingLevel())
            out += QStringLiteral("<w:pStyle w:val=\"Heading%1\"/>")
                       .arg(block.blockFormat().headingLevel());
        if (const auto* list = block.textList()) {
            if (!lists.contains(list)) {
                const int id = static_cast<int>(lists.size()) + 1;
                lists.insert(list, id);
                const auto style = list->format().style();
                const bool ordered = style <= QTextListFormat::ListDecimal;
                QString fmt = ordered ? QStringLiteral("decimal") : QStringLiteral("bullet");
                if (style == QTextListFormat::ListLowerAlpha)
                    fmt = QStringLiteral("lowerLetter");
                if (style == QTextListFormat::ListUpperAlpha)
                    fmt = QStringLiteral("upperLetter");
                if (style == QTextListFormat::ListLowerRoman)
                    fmt = QStringLiteral("lowerRoman");
                if (style == QTextListFormat::ListUpperRoman)
                    fmt = QStringLiteral("upperRoman");
                numbering += QStringLiteral("<w:abstractNum w:abstractNumId=\"%1\">"
                                            "<w:multiLevelType w:val=\"singleLevel\"/><w:lvl "
                                            "w:ilvl=\"0\"><w:start w:val=\"%2\"/>"
                                            "<w:numFmt w:val=\"%3\"/><w:lvlText "
                                            "w:val=\"%4\"/><w:pPr><w:ind w:left=\"%5\" "
                                            "w:hanging=\"360\"/></w:pPr></w:lvl></w:abstractNum>")
                                 .arg(QString::number(id), QString::number(list->format().start()),
                                      fmt, ordered ? QStringLiteral("%1.") : QString(QChar(0x2022)),
                                      QString::number(std::max(1, list->format().indent()) * 360));
                numberingInstances +=
                    QStringLiteral("<w:num w:numId=\"%1\"><w:abstractNumId w:val=\"%1\"/></w:num>")
                        .arg(id);
            }
            out += QStringLiteral("<w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"%1\"/></w:numPr>")
                       .arg(lists.value(list));
        }
        out += QStringLiteral("</w:pPr>");
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            const auto format = fragment.charFormat();
            if (format.isImageFormat()) {
                out += image(format.toImageFormat().name());
                continue;
            }
            out += QStringLiteral("<w:r><w:rPr>");
            if (format.fontWeight() >= QFont::Bold)
                out += QStringLiteral("<w:b/>");
            if (format.fontItalic())
                out += QStringLiteral("<w:i/>");
            if (format.fontUnderline())
                out += QStringLiteral("<w:u w:val=\"single\"/>");
            if (format.fontStrikeOut())
                out += QStringLiteral("<w:strike/>");
            auto text = xmlText(fragment.text());
            text.replace(QChar::LineSeparator,
                         QStringLiteral("</w:t><w:br/><w:t xml:space=\"preserve\">"));
            out += QStringLiteral("</w:rPr><w:t xml:space=\"preserve\">%1</w:t></w:r>").arg(text);
        }
        return out + QStringLiteral("</w:p>");
    }
    QString image(const QString& name) {
        const ScreenshotDocumentFormula* formula = nullptr;
        for (const auto& candidate : source.formulas)
            if (candidate.id == name) {
                formula = &candidate;
                break;
            }
        const auto raster = source.images.value(name);
        if (raster.isNull())
            return formula ? QStringLiteral("<w:r><w:t xml:space=\"preserve\">%1</w:t></w:r>")
                                 .arg(xmlText(formula->source))
                           : QString{};
        const QSizeF size = raster.deviceIndependentSize();
        const double scale = std::min(1.0, 642.0 / size.width());
        const auto cx = qRound64(size.width() * scale * 9525);
        const auto cy = qRound64(size.height() * scale * 9525);
        return QStringLiteral(
                   "<w:r><w:drawing><wp:inline><wp:extent cx=\"%1\" cy=\"%2\"/>"
                   "<wp:docPr id=\"%3\" name=\"Image %3\" descr=\"%4\"/><a:graphic><a:graphicData "
                   "uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><pic:pic>"
                   "<pic:nvPicPr><pic:cNvPr id=\"%3\" name=\"Image "
                   "%3\"/><pic:cNvPicPr/></pic:nvPicPr>"
                   "<pic:blipFill><a:blip "
                   "r:embed=\"image%5\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                   "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%1\" cy=\"%2\"/></a:xfrm>"
                   "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>"
                   "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r>")
            .arg(QString::number(cx), QString::number(cy), QString::number(++drawing),
                 xmlText(formula ? formula->source : name), QString::number(assets.value(name)));
    }
    QString frame(QTextFrame* node, const std::function<bool()>& cancelled) {
        QString out;
        for (auto it = node->begin(); !it.atEnd(); ++it) {
            if (cancelled && cancelled())
                return {};
            if (auto* table = qobject_cast<QTextTable*>(it.currentFrame())) {
                out += QStringLiteral(
                    "<w:tbl><w:tblPr><w:tblW w:w=\"5000\" w:type=\"pct\"/>"
                    "<w:tblBorders><w:top w:val=\"single\" w:sz=\"4\"/><w:left w:val=\"single\" "
                    "w:sz=\"4\"/>"
                    "<w:bottom w:val=\"single\" w:sz=\"4\"/><w:right w:val=\"single\" w:sz=\"4\"/>"
                    "<w:insideH w:val=\"single\" w:sz=\"4\"/><w:insideV w:val=\"single\" "
                    "w:sz=\"4\"/>"
                    "</w:tblBorders></w:tblPr><w:tblGrid>");
                for (int col = 0; col < table->columns(); ++col)
                    out += QStringLiteral("<w:gridCol w:w=\"%1\"/>")
                               .arg(9638 / std::max(1, table->columns()));
                out += QStringLiteral("</w:tblGrid>");
                for (int row = 0; row < table->rows(); ++row) {
                    out += QStringLiteral("<w:tr>");
                    for (int col = 0; col < table->columns();) {
                        const auto cell = table->cellAt(row, col);
                        out += QStringLiteral("<w:tc><w:tcPr>");
                        if (cell.columnSpan() > 1)
                            out +=
                                QStringLiteral("<w:gridSpan w:val=\"%1\"/>").arg(cell.columnSpan());
                        if (cell.rowSpan() > 1)
                            out += cell.row() == row
                                       ? QStringLiteral("<w:vMerge w:val=\"restart\"/>")
                                       : QStringLiteral("<w:vMerge/>");
                        out += QStringLiteral("</w:tcPr>");
                        if (cell.row() == row)
                            for (auto ci = cell.begin(); ci != cell.end(); ++ci) {
                                if (ci.currentBlock().isValid())
                                    out += paragraph(ci.currentBlock());
                                else if (ci.currentFrame())
                                    out += frame(ci.currentFrame(), cancelled);
                            }
                        out += QStringLiteral("<w:p/></w:tc>");
                        col += cell.columnSpan();
                    }
                    out += QStringLiteral("</w:tr>");
                }
                out += QStringLiteral("</w:tbl>");
            } else if (it.currentFrame())
                out += frame(it.currentFrame(), cancelled);
            else if (it.currentBlock().isValid())
                out += paragraph(it.currentBlock());
        }
        return out;
    }
    const ScreenshotRecognizedDocument& source;
    QMap<QString, QByteArray> parts;
    QHash<QString, int> assets;
    QHash<const QTextList*, int> lists;
    int drawing = 0;
    const QString office =
        QStringLiteral("http://schemas.openxmlformats.org/officeDocument/2006/relationships/");
    QString rels =
        QStringLiteral(
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"styles\" Type=\"%1styles\" Target=\"styles.xml\"/>"
            "<Relationship Id=\"numbering\" Type=\"%1numbering\" Target=\"numbering.xml\"/>")
            .arg(office);
    QString numberingInstances;
    QString numbering = QStringLiteral(
        "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">");
};
} // namespace

QString screenshotDocumentImageDirectoryName(const QString& primaryName) {
    return QFileInfo(primaryName).fileName() + QStringLiteral("_images");
}

QMap<QString, QByteArray> screenshotDocumentFiles(const ScreenshotRecognizedDocument& document,
                                                  const QString& primaryName,
                                                  const QString& extension,
                                                  const std::function<bool()>& cancelled) {
    QMap<QString, QByteArray> files;
    if (cancelled && cancelled())
        return files;
    QByteArray bytes;
    if (extension == QStringLiteral("docx"))
        bytes = WordWriter(document).write(cancelled);
    else if (extension == QStringLiteral("html"))
        bytes = document.portableHtml(cancelled).toUtf8();
    else if (extension == QStringLiteral("pdf")) {
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        {
            QPdfWriter writer(&buffer);
            writer.setPageLayout(QPageLayout(QPageSize(QPageSize::A4), QPageLayout::Portrait,
                                             QMarginsF(20, 20, 20, 20), QPageLayout::Millimeter));
            writer.setResolution(96);
            auto text = document.createTextDocument(
                false, ScreenshotRecognizedDocument::FormulaFallback::Source);
            // Paginate explicitly: QTextDocument::print otherwise adds its own 20 mm
            // margins and page numbers on top of the writer's configured margins.
            text->documentLayout()->setPaintDevice(&writer);
            text->setDocumentMargin(0);
            text->setPageSize(QSizeF(writer.width(), writer.height()));
            text->setLayoutEnabled(true);
            text->print(&writer);
        }
    } else {
        const QString directory = screenshotDocumentImageDirectoryName(primaryName);
        if (extension == QStringLiteral("md")) {
            bytes = document.markdown(directory).toUtf8();
            for (const auto& id : document.markdownImageIds())
                files.insert(directory + u'/' + id + QStringLiteral(".png"),
                             png(document.images.value(id)));
        } else if (extension == QStringLiteral("tex")) {
            bytes = document.latex(directory).toUtf8();
            for (auto it = document.images.cbegin(); it != document.images.cend(); ++it)
                files.insert(directory + u'/' + it.key() + QStringLiteral(".png"), png(it.value()));
        }
    }
    if (bytes.isEmpty() || (cancelled && cancelled()))
        return {};
    files.insert(primaryName, bytes);
    return files;
}
#endif
