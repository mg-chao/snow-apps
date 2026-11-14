#include "snow_shot/presentation/screenshotrecognizeddocument.h"
#include "snow_shot/presentation/screenshotdocumentformatting.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextImageFormat>
#include <QTextList>
#include <QTextTable>
#include <QUuid>
#include <QUrl>
#include <QImageWriter>
#include <cmath>
#include <algorithm>

#if SNOW_SHOT_ENABLE_DOCUMENT_RECOGNITION
namespace {
QString message(const char* text) {
    return QCoreApplication::translate("ScreenshotRecognizedDocument", text);
}
QString texEscape(QString text) {
    QString out;
    for (const auto c : text) {
        switch (c.unicode()) {
        case '\\':
            out += QStringLiteral("\\textbackslash{}");
            break;
        case '{':
        case '}':
        case '%':
        case '$':
        case '&':
        case '#':
        case '_':
            out += u'\\';
            out += c;
            break;
        case '^':
            out += QStringLiteral("\\textasciicircum{}");
            break;
        case '~':
            out += QStringLiteral("\\textasciitilde{}");
            break;
        default:
            out += c;
            break;
        }
    }
    return out;
}
QString formulaId(int index) {
    return QStringLiteral("snow-document-formula-%1").arg(index);
}
QString figureId(int index) {
    return QStringLiteral("snow-document-image-%1").arg(index);
}
QString imageData(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "PNG");
    writer.setQuality(75); // A low PNG compression level keeps clipboard preparation inexpensive.
    if (!writer.write(image))
        return {};
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
}
// Recognize math delimiters only outside fenced and inline code.
QString extractMath(const QString& source, QVector<ScreenshotDocumentFormula>& formulas) {
    QString out;
    bool fenced = false;
    bool code = false;
    for (qsizetype i = 0; i < source.size();) {
        if ((i == 0 || source.at(i - 1) == u'\n') && (source.mid(i, 3) == QStringLiteral("```") ||
                                                      source.mid(i, 3) == QStringLiteral("~~~"))) {
            fenced = !fenced;
            out += source.mid(i, 3);
            i += 3;
            continue;
        }
        if (!fenced && source.at(i) == u'`') {
            code = !code;
            out += source.at(i++);
            continue;
        }
        if (!fenced && !code && formulas.size() < 512 && (i == 0 || source.at(i - 1) != u'\\')) {
            QString close;
            int width = 0;
            bool display = false;
            if (source.mid(i, 2) == QStringLiteral("$$")) {
                close = QStringLiteral("$$");
                width = 2;
                display = true;
            } else if (source.mid(i, 2) == QStringLiteral("\\[")) {
                close = QStringLiteral("\\]");
                width = 2;
                display = true;
            } else if (source.mid(i, 2) == QStringLiteral("\\(")) {
                close = QStringLiteral("\\)");
                width = 2;
            } else if (source.at(i) == u'$') {
                close = QStringLiteral("$");
                width = 1;
            }
            if (width) {
                const auto search = QStringView(source).sliced(i + width).left(16'384);
                const auto relativeEnd = search.indexOf(close);
                const auto end = i + width + relativeEnd;
                if (relativeEnd > 0 && end - i <= 16'384) {
                    const QString math = source.mid(i + width, end - i - width);
                    if (display || !math.contains(u'\n')) {
                        const QString id = formulaId(static_cast<int>(formulas.size()));
                        formulas.push_back({id, math, display});
                        if (display)
                            out += QStringLiteral("\n\n");
                        out += QStringLiteral("![%1](%1)").arg(id);
                        if (display)
                            out += QStringLiteral("\n\n");
                        i = end + width;
                        continue;
                    }
                }
            }
        }
        out += source.at(i++);
    }
    return out;
}
} // namespace

std::shared_ptr<ScreenshotRecognizedDocument>
ScreenshotRecognizedDocument::parse(const QJsonObject& raw, const QImage& original, QString* error,
                                    const std::function<bool()>& cancelled) {
    auto result = std::make_shared<ScreenshotRecognizedDocument>();
    QString markdown = raw.value(QStringLiteral("md_results")).toString();
    if (markdown.size() > 8 * 1024 * 1024 || markdown.trimmed().isEmpty() || original.isNull()) {
        if (error)
            *error = message(QT_TRANSLATE_NOOP("ScreenshotRecognizedDocument",
                                               "Document recognition returned no document"));
        return {};
    }
    const auto pages =
        raw.value(QStringLiteral("data_info")).toObject().value(QStringLiteral("pages")).toArray();
    const auto page = pages.isEmpty() ? QJsonObject{} : pages.first().toObject();
    const double pageWidth = page.value(QStringLiteral("width")).toDouble(original.width());
    const double pageHeight = page.value(QStringLiteral("height")).toDouble(original.height());
    // Resolve provider image references through layout regions without downloading their URLs.
    const auto layout = raw.value(QStringLiteral("layout_details")).toArray();
    const auto regions = layout.isEmpty() ? QJsonArray{} : layout.first().toArray();
    QHash<QString, QString> imageReferences;
    for (const auto& value : regions) {
        if (cancelled && cancelled())
            return {};
        const auto region = value.toObject();
        if (region.value(QStringLiteral("label")).toString() != QStringLiteral("image"))
            continue;
        auto box = region.value(QStringLiteral("bbox_2d")).toArray();
        if (box.isEmpty())
            box = region.value(QStringLiteral("bbox")).toArray();
        if (box.size() != 4)
            continue;
        QStringList coordinates;
        bool valid = true;
        for (const auto& coordinate : box) {
            valid = valid && coordinate.isDouble() && std::isfinite(coordinate.toDouble());
            coordinates.push_back(QString::number(coordinate.toDouble(), 'g', 16));
        }
        if (!valid)
            continue;
        const auto placeholder = QStringLiteral("page=0,bbox=[%1]").arg(coordinates.join(u','));
        const auto reference = region.value(QStringLiteral("image_path")).toString();
        if (!reference.isEmpty())
            imageReferences.insert(reference, placeholder);
        const auto content = region.value(QStringLiteral("content")).toString();
        if (content.startsWith(QStringLiteral("![](")) && content.endsWith(u')'))
            imageReferences.insert(content.sliced(4, content.size() - 5), placeholder);
    }
    // The hosted API reports pixel coordinates in its returned page coordinate space.
    const QRegularExpression imageLink(QStringLiteral(R"(!\[([^\]]*)\]\(([^)]+)\))"));
    const QRegularExpression crop(QStringLiteral(R"(^page=(\d+),\s*bbox=\[([^\]]+)\]$)"));
    auto matches = imageLink.globalMatch(markdown);
    struct Replacement {
        qsizetype start;
        qsizetype length;
        QString value;
    };
    QVector<Replacement> replacements;
    qint64 imageBytes = 0;
    while (matches.hasNext()) {
        if (cancelled && cancelled())
            return {};
        const auto match = matches.next();
        const auto destination = match.captured(2);
        const auto resolved = imageReferences.value(destination, destination);
        const auto boxMatch = crop.match(resolved);
        if (!boxMatch.hasMatch())
            continue;
        const auto coordinates = boxMatch.captured(2).split(u',');
        QString replacement =
            message(QT_TRANSLATE_NOOP("ScreenshotRecognizedDocument", "[Image unavailable]"));
        if (coordinates.size() == 4 && boxMatch.captured(1) == QStringLiteral("0") &&
            pageWidth > 0 && pageHeight > 0 && result->images.size() < 512) {
            bool valid = true;
            double values[4]{};
            for (int i = 0; i < 4; ++i) {
                bool ok = false;
                values[i] = coordinates[i].trimmed().toDouble(&ok);
                valid = valid && ok && std::isfinite(values[i]);
            }
            if (valid) {
                const bool normalized =
                    values[0] >= 0 && values[1] >= 0 && values[2] <= 1 && values[3] <= 1;
                const double sx = original.width() / (normalized ? 1.0 : pageWidth);
                const double sy = original.height() / (normalized ? 1.0 : pageHeight);
                if (values[0] >= 0 && values[1] >= 0 && values[2] > values[0] &&
                    values[3] > values[1] && values[2] <= (normalized ? 1.0 : pageWidth) &&
                    values[3] <= (normalized ? 1.0 : pageHeight)) {
                    const QRect box(QPoint(static_cast<int>(std::floor(values[0] * sx)),
                                           static_cast<int>(std::floor(values[1] * sy))),
                                    QPoint(static_cast<int>(std::ceil(values[2] * sx)) - 1,
                                           static_cast<int>(std::ceil(values[3] * sy)) - 1));
                    const auto clipped = box.intersected(original.rect());
                    const auto rowBytes =
                        (qint64(clipped.width()) * original.depth() + 31) / 32 * 4;
                    if (!clipped.isEmpty() &&
                        rowBytes * clipped.height() <= maximumImageBytes - imageBytes) {
                        const QString id = figureId(static_cast<int>(result->images.size()));
                        auto image = original.copy(clipped);
                        if (!image.isNull() &&
                            image.sizeInBytes() <= maximumImageBytes - imageBytes) {
                            imageBytes += image.sizeInBytes();
                            result->images.insert(id, std::move(image));
                            replacement =
                                QStringLiteral("![%1](%2)")
                                    .arg(match.captured(1).isEmpty() ? id : match.captured(1), id);
                        }
                    }
                }
            }
        }
        replacements.push_back({match.capturedStart(), match.capturedLength(), replacement});
    }
    if (!replacements.isEmpty()) {
        QString resolved;
        resolved.reserve(markdown.size());
        qsizetype offset = 0;
        for (const auto& replacement : replacements) {
            resolved += QStringView(markdown).sliced(offset, replacement.start - offset);
            resolved += replacement.value;
            offset = replacement.start + replacement.length;
        }
        resolved += QStringView(markdown).sliced(offset);
        markdown = std::move(resolved);
    }
    markdown = extractMath(markdown, result->formulas);
    if (cancelled && cancelled())
        return {};
    snow_shot::presentation::ScreenshotInertDocument document;
    document.setLayoutEnabled(false);
    document.setUndoRedoEnabled(false);
    document.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    // Qt imports only supported rich text. Remove unresolved images and unsafe link schemes
    // before serialization so the same inert content is safe in standalone HTML and Word.
    QVector<QTextCursor> remove;
    QSet<QString> formulaIds;
    for (const auto& formula : result->formulas)
        formulaIds.insert(formula.id);
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        if (cancelled && cancelled())
            return {};
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            auto format = fragment.charFormat();
            if (!format.isImageFormat() && !format.isAnchor())
                continue;
            QTextCursor cursor(&document);
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            if (format.isImageFormat()) {
                const auto name = format.toImageFormat().name();
                if (!result->images.contains(name) && !formulaIds.contains(name))
                    remove.push_back(cursor);
            }
            if (format.isAnchor()) {
                const auto scheme = QUrl(format.anchorHref()).scheme();
                if (scheme != QStringLiteral("https") && scheme != QStringLiteral("http") &&
                    scheme != QStringLiteral("mailto")) {
                    format.setAnchor(false);
                    format.setAnchorHref({});
                    cursor.setCharFormat(format);
                }
            }
        }
    }
    for (auto it = remove.rbegin(); it != remove.rend(); ++it)
        it->insertText(
            message(QT_TRANSLATE_NOOP("ScreenshotRecognizedDocument", "[Image unavailable]")));
    result->html = document.toHtml();
    return result;
}

std::unique_ptr<QTextDocument>
ScreenshotRecognizedDocument::createTextDocument(bool layout, FormulaFallback fallback) const {
    auto document = std::make_unique<snow_shot::presentation::ScreenshotInertDocument>();
    document->setLayoutEnabled(false);
    document->setUndoRedoEnabled(false);
    document->setDefaultStyleSheet(
        QStringLiteral("body {color:#202020;background-color:#ffffff;} p {margin:6px 0;} table "
                       "{border-collapse:collapse;} "
                       "td,th {border:1px solid #999;padding:5px;}"));
    document->setHtml(html);
    auto textOption = document->defaultTextOption();
    textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document->setDefaultTextOption(textOption);
    std::function<void(QTextFrame*)> fitTables = [&](QTextFrame* frame) {
        for (auto* child : frame->childFrames()) {
            if (auto* table = qobject_cast<QTextTable*>(child)) {
                auto format = table->format();
                format.setWidth(QTextLength(QTextLength::PercentageLength, 100));
                format.clearColumnWidthConstraints();
                table->setFormat(format);
            }
            fitTables(child);
        }
    };
    fitTables(document->rootFrame());
    updateTextDocumentImages(*document, fallback);
    if (layout)
        document->setLayoutEnabled(true);
    return document;
}

void ScreenshotRecognizedDocument::updateTextDocumentImages(QTextDocument& document,
                                                            FormulaFallback fallback) const {
    QHash<QString, QString> sources;
    for (const auto& formula : formulas)
        sources.insert(formula.id, formula.source);
    for (auto it = images.cbegin(); it != images.cend(); ++it)
        document.addResource(QTextDocument::ImageResource, QUrl(it.key()), it.value());
    QTextCursor cursor(&document);
    cursor.beginEditBlock();
    QVector<QPair<QTextCursor, QString>> missingFormulas;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.charFormat().isImageFormat())
                continue;
            const auto previous = fragment.charFormat();
            auto format = previous.toImageFormat();
            const auto source = sources.constFind(format.name());
            if (source != sources.cend())
                format.setProperty(QTextFormat::ImageAltText, *source);
            const auto image = images.value(format.name());
            if (!image.isNull()) {
                QSizeF size = image.deviceIndependentSize();
                if (size.width() > 600)
                    size *= 600 / size.width();
                format.setWidth(size.width());
                format.setHeight(size.height());
                format.setMaximumWidth(QTextLength(QTextLength::PercentageLength, 100));
            }
            const bool useSource =
                fallback == FormulaFallback::Source && image.isNull() && source != sources.cend();
            if (previous != format || useSource) {
                cursor.setPosition(fragment.position());
                cursor.setPosition(fragment.position() + fragment.length(),
                                   QTextCursor::KeepAnchor);
                if (useSource)
                    missingFormulas.push_back({cursor, *source});
                else
                    cursor.setCharFormat(format);
            }
        }
    }
    for (auto it = missingFormulas.rbegin(); it != missingFormulas.rend(); ++it)
        it->first.insertText(it->second, QTextCharFormat{});
    cursor.endEditBlock();
}

namespace {
QString embedImages(const QString& html, const QHash<QString, QImage>& images,
                    const std::function<bool()>& cancelled) {
    static const QRegularExpression imageSource(QStringLiteral(R"re(<img[^>]*\ssrc="([^"]+)")re"));
    QHash<qint64, QString> encoded;
    QString output;
    output.reserve(html.size());
    qsizetype offset = 0;
    auto matches = imageSource.globalMatch(html);
    while (matches.hasNext()) {
        if (cancelled && cancelled())
            return {};
        const auto match = matches.next();
        const QString id = match.captured(1);
        const auto image = images.constFind(id);
        if (image == images.cend())
            continue;
        const auto rasterKey = image->cacheKey();
        const auto found = encoded.constFind(rasterKey);
        const auto data = found == encoded.cend() ? imageData(*image) : *found;
        if (found == encoded.cend())
            encoded.insert(rasterKey, data);
        output += QStringView(html).sliced(offset, match.capturedStart(1) - offset);
        output += data;
        offset = match.capturedEnd(1);
    }
    output += QStringView(html).sliced(offset);
    return output;
}

QString documentPlainText(QTextDocument& document) {
    QVector<QTextCursor> positions;
    QStringList sources;
    for (auto block = document.begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            if (!fragment.charFormat().isImageFormat())
                continue;
            QTextCursor cursor(&document);
            cursor.setPosition(fragment.position());
            cursor.setPosition(fragment.position() + fragment.length(), QTextCursor::KeepAnchor);
            positions.push_back(cursor);
            sources.push_back(fragment.charFormat().stringProperty(QTextFormat::ImageAltText));
        }
    }
    for (qsizetype i = positions.size(); i > 0; --i)
        positions[i - 1].insertText(sources[i - 1]);
    return document.toPlainText();
}
} // namespace

QString ScreenshotRecognizedDocument::portableHtml(const std::function<bool()>& cancelled) const {
    return preparedClipboard
               ? preparedClipboard->html
               : embedImages(createTextDocument(false, FormulaFallback::Source)->toHtml(), images,
                             cancelled);
}

QString ScreenshotRecognizedDocument::plainText() const {
    return preparedClipboard ? preparedClipboard->text
                             : documentPlainText(*createTextDocument(false));
}

ScreenshotDocumentClipboard
ScreenshotRecognizedDocument::clipboardContent(const std::function<bool()>& cancelled) const {
    if (preparedClipboard)
        return *preparedClipboard;
    auto document = createTextDocument(false, FormulaFallback::Source);
    auto portable = embedImages(document->toHtml(), images, cancelled);
    if (cancelled && cancelled())
        return {};
    return {std::move(portable), documentPlainText(*document)};
}

QSet<QString> ScreenshotRecognizedDocument::markdownImageIds() const {
    static const QRegularExpression imageSource(QStringLiteral(R"re(<img[^>]*\ssrc="([^"]+)")re"));
    QSet<QString> ids;
    auto matches = imageSource.globalMatch(html);
    while (matches.hasNext()) {
        const auto id = matches.next().captured(1);
        if (images.contains(id))
            ids.insert(id);
    }
    for (const auto& formula : formulas)
        ids.remove(formula.id);
    return ids;
}

QString ScreenshotRecognizedDocument::markdown(const QString& imageDirectory) const {
    const auto imageIds = markdownImageIds();
    const auto directory = QString::fromLatin1(QUrl::toPercentEncoding(imageDirectory));
    const auto replaceAssets = [&](QString& content, bool richText) {
        const auto reference = richText ? QStringLiteral(R"re((?<=\ssrc=")%1(?="))re")
                                        : QStringLiteral(R"((?<=\]\()%1(?=[ )]))");
        for (const auto& id : imageIds) {
            const auto path = directory + u'/' + id + QStringLiteral(".png");
            content.replace(QRegularExpression(reference.arg(QRegularExpression::escape(id))),
                            path);
        }
        for (const auto& formula : formulas) {
            const auto pattern = richText ? QStringLiteral(R"re(<img[^>]*\ssrc="%1"[^>]*>)re")
                                          : QStringLiteral(R"(!\[%1\]\(%1\))");
            const QRegularExpression image(pattern.arg(QRegularExpression::escape(formula.id)));
            const QString delimiters = formula.display ? QStringLiteral("$$") : QStringLiteral("$");
            const auto source = richText ? formula.source.toHtmlEscaped() : formula.source;
            content.replace(image, delimiters + source + delimiters);
        }
    };

    // Qt's pipe tables cannot represent spans. Retain merged tables as HTML blocks while
    // Qt converts the surrounding content and ordinary tables to Markdown.
    static const QRegularExpression tableTags(QStringLiteral(R"(<table(?:\s[^>]*)?>|</table>)"));
    static const QRegularExpression cellSpan(QStringLiteral(R"(<td\s[^>]*(?:rowspan|colspan)=")"));
    static const QRegularExpression blankLines(QStringLiteral(R"(\n(?:[ \t]*\n)+)"));
    struct TableReplacement {
        qsizetype start;
        qsizetype length;
        QString placeholder;
        QString html;
    };
    QVector<TableReplacement> replacements;
    const auto placeholderPrefix =
        QStringLiteral("SnowShotMergedTable") + QUuid::createUuid().toString(QUuid::Id128);
    auto matches = tableTags.globalMatch(html);
    qsizetype tableStart = 0;
    int depth = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (match.capturedView().startsWith(u"<table")) {
            if (depth++ == 0)
                tableStart = match.capturedStart();
        } else if (--depth == 0) {
            const auto length = match.capturedEnd() - tableStart;
            auto table = html.mid(tableStart, length);
            if (table.contains(cellSpan)) {
                replaceAssets(table, true);
                // A blank line terminates a Markdown HTML block, even within a table.
                table.replace(blankLines, QStringLiteral("\n"));
                replacements.push_back(
                    {tableStart, length,
                     placeholderPrefix + QString::number(replacements.size()) + u'X',
                     std::move(table)});
            }
        }
    }
    QString source = html;
    for (auto it = replacements.crbegin(); it != replacements.crend(); ++it)
        source.replace(it->start, it->length, QStringLiteral("<p>%1</p>").arg(it->placeholder));
    snow_shot::presentation::ScreenshotInertDocument document;
    document.setHtml(source);
    QString output = document.toMarkdown();
    replaceAssets(output, false);
    for (const auto& table : replacements)
        output.replace(table.placeholder,
                       QStringLiteral("\n\n") + table.html + QStringLiteral("\n\n"));
    return output;
}

QString ScreenshotRecognizedDocument::latex(const QString& imageDirectory) const {
    auto document = createTextDocument();
    QString output = QStringLiteral(
        "\\documentclass[UTF8,fontset=fandol]{ctexart}\n\\usepackage[a4paper,margin=20mm]{geometry}"
        "\n\\usepackage{amsmath,amssymb,graphicx,longtable,multirow}\n\\begin{document}\n");
    const auto blockText = [&](const QTextBlock& block) {
        QString text;
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            const auto format = fragment.charFormat();
            if (format.isImageFormat()) {
                const auto id = format.toImageFormat().name();
                bool formulaFound = false;
                for (const auto& formula : formulas)
                    if (formula.id == id) {
                        // Reuse the preview raster; provider commands remain inert in exports.
                        const auto raster = images.value(id);
                        if (!raster.isNull()) {
                            const auto width =
                                std::min(450.0, raster.deviceIndependentSize().width() * 0.75);
                            const auto image = QStringLiteral("\\includegraphics[width=%1pt,"
                                                              "keepaspectratio]{%2/%3.png}")
                                                   .arg(width, 0, 'f', 2)
                                                   .arg(imageDirectory, id);
                            text +=
                                formula.display
                                    ? QStringLiteral("\\begin{center}%1\\end{center}").arg(image)
                                    : image;
                        } else {
                            text += QStringLiteral("\\texttt{%1}").arg(texEscape(formula.source));
                        }
                        formulaFound = true;
                        break;
                    }
                if (!formulaFound && images.contains(id))
                    text +=
                        QStringLiteral(
                            "\\includegraphics[width=0.8\\linewidth,keepaspectratio]{%1/%2.png}")
                            .arg(imageDirectory, id);
            } else {
                QString run = texEscape(fragment.text());
                if (format.fontWeight() >= QFont::Bold)
                    run = QStringLiteral("\\textbf{%1}").arg(run);
                if (format.fontItalic())
                    run = QStringLiteral("\\textit{%1}").arg(run);
                text += run;
            }
        }
        return text;
    };
    std::function<void(QTextFrame*)> frame;
    frame = [&](QTextFrame* node) {
        QStringList listKinds;
        QVector<const QTextList*> listOwners;
        const auto closeLists = [&](int depth) {
            while (listKinds.size() > depth) {
                output += QStringLiteral("\\end{%1}\n").arg(listKinds.takeLast());
                listOwners.removeLast();
            }
        };
        for (auto it = node->begin(); !it.atEnd(); ++it) {
            const auto block = it.currentBlock();
            if (!block.isValid() || !block.textList())
                closeLists(0);
            if (auto* table = qobject_cast<QTextTable*>(it.currentFrame())) {
                output += QStringLiteral("\\begin{longtable}{%1}\n")
                              .arg(QStringLiteral("p{%1\\linewidth}")
                                       .arg(0.9 / std::max(1, table->columns()))
                                       .repeated(table->columns()));
                for (int row = 0; row < table->rows(); ++row) {
                    for (int col = 0; col < table->columns();) {
                        if (col)
                            output += QStringLiteral(" & ");
                        const auto cell = table->cellAt(row, col);
                        QString content;
                        if (cell.row() == row && cell.column() == col)
                            for (auto ci = cell.begin(); ci != cell.end(); ++ci)
                                if (ci.currentBlock().isValid())
                                    content += blockText(ci.currentBlock()) + u' ';
                        if (cell.rowSpan() > 1 && cell.row() == row)
                            content = QStringLiteral("\\multirow{%1}{*}{%2}")
                                          .arg(cell.rowSpan())
                                          .arg(content);
                        if (cell.columnSpan() > 1)
                            content = QStringLiteral("\\multicolumn{%1}{l}{%2}")
                                          .arg(cell.columnSpan())
                                          .arg(content);
                        output += content;
                        col += cell.columnSpan();
                    }
                    output += QStringLiteral(" \\\\\n");
                }
                output += QStringLiteral("\\end{longtable}\n");
            } else if (it.currentFrame())
                frame(it.currentFrame());
            else if (block.isValid()) {
                const auto text = blockText(block);
                const int heading = block.blockFormat().headingLevel();
                if (heading)
                    output += QStringLiteral("\\%1*{%2}\n")
                                  .arg(heading == 1   ? QStringLiteral("section")
                                       : heading == 2 ? QStringLiteral("subsection")
                                                      : QStringLiteral("subsubsection"),
                                       text);
                else if (const auto* list = block.textList()) {
                    const int depth = std::clamp(list->format().indent(), 1, 4);
                    closeLists(depth);
                    if (listOwners.size() == depth && listOwners.constLast() != list)
                        closeLists(depth - 1);
                    while (listKinds.size() < depth) {
                        const bool ordered = list->format().style() <= QTextListFormat::ListDecimal;
                        const auto kind =
                            ordered ? QStringLiteral("enumerate") : QStringLiteral("itemize");
                        output += QStringLiteral("\\begin{%1}\n").arg(kind);
                        listKinds.push_back(kind);
                        listOwners.push_back(list);
                        if (listKinds.size() < depth)
                            output += QStringLiteral("\\item ");
                    }
                    output += QStringLiteral("\\item %1\n").arg(text);
                } else
                    output += text + QStringLiteral("\n\n");
            }
        }
        closeLists(0);
    };
    frame(document->rootFrame());
    output += QStringLiteral("\\end{document}\n");
    // Preserve sources as comments, including multiline formulas, without executing them.
    for (const auto& formula : formulas) {
        output += QStringLiteral("% %1\n").arg(formula.id);
        auto source = formula.source;
        source.replace(u'\r', u'\n');
        for (const auto& line : source.split(u'\n'))
            output += QStringLiteral("% %1\n").arg(line);
    }
    return output;
}
qint64 ScreenshotRecognizedDocument::retainedBytes() const {
    qint64 size = html.size() * 2;
    if (preparedClipboard)
        size += (preparedClipboard->html.size() + preparedClipboard->text.size()) * 2;
    for (const auto& image : images)
        size += image.sizeInBytes();
    for (const auto& formula : formulas)
        size += formula.source.size() * 2;
    return size;
}
#endif
