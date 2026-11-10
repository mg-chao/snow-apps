#include "snow_shot/presentation/screenshottabledocument.h"
#include "snow_shot/presentation/screenshotdocumentformatting.h"

#include <QRegularExpression>
#include <QCryptographicHash>
#include <QBitArray>
#include <QMimeData>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextFrame>
#include <QTextTable>

#include <algorithm>

namespace {
const QString tableClipboardFormat = QStringLiteral("application/x-snow-shot-table-html");
constexpr int maximumTableDimension = 256;
constexpr int maximumTableCells = 16384;

bool needsSpreadsheetTextPrefix(const QString& text) {
    static const QRegularExpression numericText(QStringLiteral("\\A[+-]?[0-9]+(?:\\.[0-9]+)?\\z"));
    if (!numericText.match(text.trimmed()).hasMatch()) {
        return false;
    }
    // General-format spreadsheet cells may use scientific notation from 12 digits;
    // longer values can also exceed Excel's 15-digit precision. Never parse as a double.
    return std::count_if(text.cbegin(), text.cend(), [](QChar character) {
               return character >= QLatin1Char('0') && character <= QLatin1Char('9');
           }) >= 12;
}

void collectTables(QTextFrame* frame, QVector<QTextTable*>* tables) {
    if (frame == nullptr || tables == nullptr) {
        return;
    }
    for (QTextFrame::iterator iterator = frame->begin(); !iterator.atEnd(); ++iterator) {
        QTextFrame* child = iterator.currentFrame();
        if (child == nullptr) {
            continue;
        }
        if (auto* table = qobject_cast<QTextTable*>(child)) {
            tables->push_back(table);
            continue;
        }
        collectTables(child, tables);
    }
}

QString tableCellText(const QTextTableCell& cell) {
    if (!cell.isValid()) {
        return {};
    }
    QTextCursor cursor = cell.firstCursorPosition();
    cursor.setPosition(cell.lastCursorPosition().position(), QTextCursor::KeepAnchor);
    return QTextDocumentFragment(cursor)
        .toPlainText()
        .replace(QChar(0x2029), QLatin1Char('\n'))
        .trimmed();
}

QString escapedCellText(const QString& text) {
    return text.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br/>"));
}

quint64 coordinateKey(int row, int column) {
    return (static_cast<quint64>(static_cast<quint32>(row)) << 32U) | static_cast<quint32>(column);
}

QString boundedTableHtml(const QString& source, QBitArray& cellHeaders) {
    // Normalize only table structure. Missing end tags, wrappers and unrelated markup do not
    // invalidate a usable table. Bound the expanded grid before Qt allocates any table cells.
    static const QRegularExpression markup(QStringLiteral(
        "<!--[\\s\\S]*?-->|</?([A-Za-z][A-Za-z0-9]*)\\b(?:[^<>\"']|\"[^\"]*\"|'[^']*')*/?>"));
    static const QRegularExpression span(
        QStringLiteral("\\b(rowspan|colspan)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))"),
        QRegularExpression::CaseInsensitiveOption);
    QBitArray occupied(maximumTableDimension * maximumTableDimension);
    cellHeaders.resize(occupied.size());
    QString output;
    QString cellTag;
    int depth = 0;
    int row = -1;
    int column = 0;
    int gridRows = 0;
    int gridColumns = 0;
    int cells = 0;
    bool inRow = false;
    QString ignored;
    qsizetype position = 0;
    const auto appendText = [&](QStringView text) {
        // Leave entities for Qt to decode, but never let an unmatched/incomplete tag in
        // cell text bypass the allocation bounds enforced on structural tokens below.
        output += text.toString()
                      .replace(u'<', QStringLiteral("&lt;"))
                      .replace(u'>', QStringLiteral("&gt;"));
    };
    const auto closeCell = [&] {
        if (!cellTag.isEmpty()) {
            output += QStringLiteral("</") + cellTag + u'>';
            cellTag.clear();
        }
    };
    const auto closeRow = [&] {
        closeCell();
        if (inRow)
            output += QStringLiteral("</tr>");
        inRow = false;
    };
    const auto startRow = [&] {
        closeRow();
        ++row;
        column = 0;
        inRow = true;
        output += QStringLiteral("<tr>");
    };
    auto matches = markup.globalMatch(source);
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (!cellTag.isEmpty() && ignored.isEmpty())
            appendText(QStringView(source).mid(position, match.capturedStart() - position));
        position = match.capturedEnd();
        const QString tag = match.captured(1).toLower();
        const bool closing = match.capturedView().startsWith(u"</");
        if (!ignored.isEmpty()) {
            if (closing && tag == ignored)
                ignored.clear();
            continue;
        }
        if (tag == u"script" || tag == u"style") {
            if (!closing)
                ignored = tag;
            continue;
        }
        if (tag == u"table") {
            if (!closing) {
                if (++depth == 1)
                    output += QStringLiteral("<table>");
                if (depth > 32)
                    return {};
            } else if (depth > 0 && --depth == 0) {
                if (cells > 0)
                    break;
                output.clear();
                inRow = false;
                row = -1;
            }
            continue;
        }
        if (depth == 0)
            continue;
        if (depth > 1) {
            if (!cellTag.isEmpty() && (tag == u"tr" || tag == u"td" || tag == u"th"))
                output += QStringLiteral("<br>");
            continue;
        }
        if (tag == u"tr") {
            if (closing)
                closeRow();
            else
                startRow();
            if (row >= maximumTableDimension)
                return {};
        } else if (tag == u"td" || tag == u"th") {
            closeCell();
            if (closing)
                continue;
            if (!inRow)
                startRow();
            if (row >= maximumTableDimension || ++cells > maximumTableCells)
                return {};
            int rowSpan = 1;
            int columnSpan = 1;
            auto spans = span.globalMatch(match.captured());
            while (spans.hasNext()) {
                const auto attribute = spans.next();
                const QString value = attribute.captured(2).isNull()
                                          ? (attribute.captured(3).isNull() ? attribute.captured(4)
                                                                            : attribute.captured(3))
                                          : attribute.captured(2);
                bool valid = false;
                const qlonglong count = value.toLongLong(&valid);
                // Invalid/zero spans use HTML's default; excessive spans are a resource limit.
                if ((valid && count > maximumTableDimension) ||
                    (!valid && value.size() > 9 &&
                     std::all_of(value.cbegin(), value.cend(),
                                 [](QChar c) { return c.isDigit(); })))
                    return {};
                const int bounded = valid && count > 0 ? static_cast<int>(count) : 1;
                (attribute.captured(1).compare(u"rowspan", Qt::CaseInsensitive) == 0 ? rowSpan
                                                                                     : columnSpan) =
                    bounded;
            }
            while (column + columnSpan <= maximumTableDimension) {
                bool available = true;
                for (int c = column; c < column + columnSpan; ++c)
                    available = available && !occupied.testBit(row * maximumTableDimension + c);
                if (available)
                    break;
                if (!occupied.testBit(row * maximumTableDimension + column))
                    output += QStringLiteral("<td></td>");
                ++column;
            }
            if (row + rowSpan > maximumTableDimension ||
                column + columnSpan > maximumTableDimension)
                return {};
            gridRows = std::max(gridRows, row + rowSpan);
            gridColumns = std::max(gridColumns, column + columnSpan);
            if (gridRows * gridColumns > maximumTableCells)
                return {};
            cellHeaders.setBit(row * maximumTableDimension + column, tag == u"th");
            for (int r = row; r < row + rowSpan; ++r)
                for (int c = column; c < column + columnSpan; ++c)
                    occupied.setBit(r * maximumTableDimension + c);
            column += columnSpan;
            cellTag = tag;
            output += QStringLiteral("<%1 rowspan=\"%2\" colspan=\"%3\">")
                          .arg(tag)
                          .arg(rowSpan)
                          .arg(columnSpan);
        } else if (!cellTag.isEmpty() && tag == u"br") {
            output += QStringLiteral("<br>");
        } else if (!cellTag.isEmpty() && (tag == u"p" || tag == u"div")) {
            // Keep block semantics without attributes so Qt treats adjacent closing/opening
            // tags as one paragraph boundary, while explicit br elements retain blank lines.
            output += closing ? QStringLiteral("</%1>").arg(tag) : QStringLiteral("<%1>").arg(tag);
        }
    }
    if (!cellTag.isEmpty() && depth > 0 && ignored.isEmpty())
        appendText(QStringView(source).mid(position));
    closeRow();
    output += QStringLiteral("</table>");
    return cells > 0 ? output : QString{};
}
} // namespace

ScreenshotTableDocument::ScreenshotTableDocument(int rows, int columns, bool firstRowIsHeader) {
    initializeSlots(rows, columns, firstRowIsHeader);
}

ScreenshotTableDocument ScreenshotTableDocument::fromHtml(const QString& source,
                                                          bool requireTable) {
    QBitArray cellHeaders;
    if (requireTable && source.size() > 4 * 1024 * 1024)
        return {};
    const QString html = requireTable ? boundedTableHtml(source, cellHeaders) : source;
    if (requireTable && html.isEmpty())
        return {};
    snow_shot::presentation::ScreenshotInertDocument htmlDocument;
    htmlDocument.setHtml(html);

    QVector<QTextTable*> tables;
    collectTables(htmlDocument.rootFrame(), &tables);
    if (tables.isEmpty()) {
        return requireTable ? ScreenshotTableDocument{} : fromPlainText(htmlDocument.toPlainText());
    }

    const QTextTable* table = tables.constFirst();
    if (requireTable && (tables.size() != 1 || table->rows() > maximumTableDimension ||
                         table->columns() > maximumTableDimension ||
                         qint64(table->rows()) * table->columns() > maximumTableCells)) {
        return {};
    }
    const bool firstRowHeader =
        !requireTable && source.contains(QRegularExpression(
                             QStringLiteral("<th\\b"), QRegularExpression::CaseInsensitiveOption));
    ScreenshotTableDocument result(table->rows(), table->columns(), firstRowHeader);
    for (int row = 0; row < table->rows(); ++row) {
        for (int column = 0; column < table->columns(); ++column) {
            const QTextTableCell sourceCell = table->cellAt(row, column);
            if (!sourceCell.isValid() || sourceCell.row() != row || sourceCell.column() != column) {
                continue;
            }
            Slot& target = result.m_slots[row][column];
            target.cell.text = tableCellText(sourceCell);
            target.cell.rowSpan = std::max(1, sourceCell.rowSpan());
            target.cell.columnSpan = std::max(1, sourceCell.columnSpan());
            target.cell.header = requireTable
                                     ? cellHeaders.testBit(row * maximumTableDimension + column)
                                     : firstRowHeader && row == 0;
            result.applySpan(row, column);
        }
    }
    return result;
}

ScreenshotTableDocument ScreenshotTableDocument::fromModelOutput(const QString& source) {
    if (source.size() > 4 * 1024 * 1024)
        return {};
    static const QRegularExpression table(QStringLiteral("<table\\b"),
                                          QRegularExpression::CaseInsensitiveOption);
    if (table.match(source).hasMatch())
        return fromHtml(source, true);
    // Markdown tables can arrive inside a code fence. Only strip fence delimiter lines;
    // never interpret arbitrary prose as a single-cell table.
    static const QRegularExpression fence(QStringLiteral("^\\s*(?:`{3,}|~{3,})[^\\n]*$"));
    QString markdown;
    int rows = 0;
    int cells = 0;
    int columns = 0;
    for (const auto& line : QStringView(source).split(u'\n')) {
        if (fence.matchView(line).hasMatch())
            continue;
        const int pipes = static_cast<int>(line.count(u'|'));
        if (pipes > 0) {
            if (++rows > maximumTableDimension || pipes > maximumTableDimension + 1)
                return {};
            const auto trimmed = line.trimmed();
            columns = std::max(columns, pipes + 1 - int(trimmed.startsWith(u'|')) -
                                            int(trimmed.endsWith(u'|')));
            cells += pipes;
            // Ragged Markdown rows are padded to the widest row by Qt. Bound that
            // expansion before setMarkdown(), not just the source's explicit cells.
            if (columns > maximumTableDimension || rows * columns > maximumTableCells ||
                cells > maximumTableCells + 2 * maximumTableDimension)
                return {};
        }
        markdown += line;
        markdown += u'\n';
    }
    if (rows < 2)
        return {};
    snow_shot::presentation::ScreenshotInertDocument document;
    document.setMarkdown(markdown, QTextDocument::MarkdownDialectGitHub);
    auto result = fromHtml(document.toHtml(), true);
    // Qt's HTML exporter represents Markdown headers as styled td elements. Preserve the
    // GFM header semantics independently of those presentation styles.
    if (!result.empty())
        for (auto& slot : result.m_slots[0])
            slot.cell.header = true;
    return result;
}

ScreenshotTableDocument ScreenshotTableDocument::fromPlainText(const QString& source) {
    QString normalized = source;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    while (normalized.endsWith(QLatin1Char('\n'))) {
        normalized.chop(1);
    }
    if (normalized.trimmed().isEmpty()) {
        return {};
    }

    const QStringList rows = normalized.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    int columns = 1;
    QVector<QStringList> values;
    values.reserve(rows.size());
    for (const QString& row : rows) {
        values.push_back(row.split(QLatin1Char('\t'), Qt::KeepEmptyParts));
        columns = std::max(columns, static_cast<int>(values.constLast().size()));
    }

    ScreenshotTableDocument result(static_cast<int>(values.size()), columns, false);
    for (int row = 0; row < values.size(); ++row) {
        for (int column = 0; column < values.at(row).size(); ++column) {
            result.m_slots[row][column].cell.text = values.at(row).at(column);
        }
    }
    return result;
}

int ScreenshotTableDocument::rowCount() const {
    return static_cast<int>(m_slots.size());
}

int ScreenshotTableDocument::columnCount() const {
    return m_slots.isEmpty() ? 0 : static_cast<int>(m_slots.constFirst().size());
}

bool ScreenshotTableDocument::empty() const {
    return rowCount() == 0 || columnCount() == 0;
}

bool ScreenshotTableDocument::firstRowIsHeader() const {
    if (empty()) {
        return false;
    }
    for (int column = 0; column < columnCount(); ++column) {
        if (isAnchor(0, column) && m_slots.at(0).at(column).cell.header) {
            return true;
        }
    }
    return false;
}

QPoint ScreenshotTableDocument::anchorAt(int row, int column) const {
    if (!contains(row, column)) {
        return QPoint(-1, -1);
    }
    const Slot& slot = m_slots.at(row).at(column);
    return QPoint(slot.anchorColumn, slot.anchorRow);
}

bool ScreenshotTableDocument::isAnchor(int row, int column) const {
    return contains(row, column) && m_slots.at(row).at(column).anchorRow == row &&
           m_slots.at(row).at(column).anchorColumn == column;
}

const ScreenshotTableCell* ScreenshotTableDocument::anchorCellAt(int row, int column) const {
    return isAnchor(row, column) ? &m_slots.at(row).at(column).cell : nullptr;
}

const ScreenshotTableCell* ScreenshotTableDocument::cellAt(int row, int column) const {
    const QPoint anchor = anchorAt(row, column);
    return anchor.x() >= 0 ? anchorCellAt(anchor.y(), anchor.x()) : nullptr;
}

QString ScreenshotTableDocument::cellText(int row, int column) const {
    const ScreenshotTableCell* cell = cellAt(row, column);
    return cell != nullptr ? cell->text : QString();
}

ScreenshotTableRange ScreenshotTableDocument::spanRangeAt(int row, int column) const {
    const QPoint anchor = anchorAt(row, column);
    const ScreenshotTableCell* cell =
        anchor.x() >= 0 ? anchorCellAt(anchor.y(), anchor.x()) : nullptr;
    if (cell == nullptr) {
        return {};
    }
    return ScreenshotTableRange{anchor.y(), anchor.x(),
                                std::min(rowCount() - 1, anchor.y() + cell->rowSpan - 1),
                                std::min(columnCount() - 1, anchor.x() + cell->columnSpan - 1)};
}

ScreenshotTableRange
ScreenshotTableDocument::expandedRange(const ScreenshotTableRange& source) const {
    ScreenshotTableRange range = boundedRange(source);
    if (!range.isValid()) {
        return {};
    }

    bool changed = true;
    while (changed) {
        changed = false;
        const ScreenshotTableRange before = range;
        for (int row = before.top; row <= before.bottom; ++row) {
            for (int column = before.left; column <= before.right; ++column) {
                const ScreenshotTableRange span = spanRangeAt(row, column);
                if (!span.isValid()) {
                    continue;
                }
                range.top = std::min(range.top, span.top);
                range.left = std::min(range.left, span.left);
                range.bottom = std::max(range.bottom, span.bottom);
                range.right = std::max(range.right, span.right);
            }
        }
        range = boundedRange(range);
        changed = range != before;
    }
    return range;
}

bool ScreenshotTableDocument::setCellText(int row, int column, const QString& text) {
    m_contentCheckpoint.clear();
    const QPoint anchor = anchorAt(row, column);
    if (anchor.x() < 0) {
        return false;
    }
    ScreenshotTableCell& cell = m_slots[anchor.y()][anchor.x()].cell;
    if (cell.text == text) {
        return false;
    }
    cell.text = text;
    return true;
}

bool ScreenshotTableDocument::canMerge(const ScreenshotTableRange& source) const {
    const ScreenshotTableRange range = expandedRange(source);
    return range.isValid() && (range.rowCount() > 1 || range.columnCount() > 1);
}

bool ScreenshotTableDocument::merge(const ScreenshotTableRange& source) {
    m_contentCheckpoint.clear();
    const ScreenshotTableRange range = expandedRange(source);
    if (!canMerge(range)) {
        return false;
    }

    QStringList values;
    QSet<quint64> visited;
    for (int row = range.top; row <= range.bottom; ++row) {
        for (int column = range.left; column <= range.right; ++column) {
            const QPoint anchor = anchorAt(row, column);
            if (anchor.x() < 0 || visited.contains(coordinateKey(anchor.y(), anchor.x()))) {
                continue;
            }
            visited.insert(coordinateKey(anchor.y(), anchor.x()));
            const QString value = cellText(anchor.y(), anchor.x()).trimmed();
            if (!value.isEmpty()) {
                values.push_back(value);
            }
        }
    }

    const bool headerRow = firstRowIsHeader();
    for (int row = range.top; row <= range.bottom; ++row) {
        for (int column = range.left; column <= range.right; ++column) {
            Slot& slot = m_slots[row][column];
            slot = Slot{};
            slot.anchorRow = row;
            slot.anchorColumn = column;
            slot.cell.header = headerRow && row == 0;
        }
    }
    Slot& anchor = m_slots[range.top][range.left];
    anchor.cell.text = values.join(QLatin1Char('\n'));
    anchor.cell.rowSpan = range.rowCount();
    anchor.cell.columnSpan = range.columnCount();
    anchor.cell.header = headerRow && range.top == 0;
    applySpan(range.top, range.left);
    return true;
}

bool ScreenshotTableDocument::canSplit(const ScreenshotTableRange& source) const {
    const ScreenshotTableRange range = boundedRange(source);
    if (!range.isValid()) {
        return false;
    }
    QSet<quint64> visited;
    for (int row = range.top; row <= range.bottom; ++row) {
        for (int column = range.left; column <= range.right; ++column) {
            const QPoint anchor = anchorAt(row, column);
            if (anchor.x() < 0 || visited.contains(coordinateKey(anchor.y(), anchor.x()))) {
                continue;
            }
            visited.insert(coordinateKey(anchor.y(), anchor.x()));
            const ScreenshotTableCell* cell = anchorCellAt(anchor.y(), anchor.x());
            if (cell != nullptr && (cell->rowSpan > 1 || cell->columnSpan > 1)) {
                return true;
            }
        }
    }
    return false;
}

bool ScreenshotTableDocument::split(const ScreenshotTableRange& source) {
    m_contentCheckpoint.clear();
    const ScreenshotTableRange range = boundedRange(source);
    if (!canSplit(range)) {
        return false;
    }

    const bool headerRow = firstRowIsHeader();
    QVector<QPoint> anchors;
    QSet<quint64> visited;
    for (int row = range.top; row <= range.bottom; ++row) {
        for (int column = range.left; column <= range.right; ++column) {
            const QPoint anchor = anchorAt(row, column);
            const quint64 key = coordinateKey(anchor.y(), anchor.x());
            if (anchor.x() < 0 || visited.contains(key)) {
                continue;
            }
            visited.insert(key);
            const ScreenshotTableCell* cell = anchorCellAt(anchor.y(), anchor.x());
            if (cell != nullptr && (cell->rowSpan > 1 || cell->columnSpan > 1)) {
                anchors.push_back(anchor);
            }
        }
    }
    for (const QPoint& anchor : anchors) {
        splitAnchor(anchor.y(), anchor.x(), headerRow);
    }
    return true;
}

bool ScreenshotTableDocument::clear(const ScreenshotTableRange& source) {
    m_contentCheckpoint.clear();
    const ScreenshotTableRange range = boundedRange(source);
    if (!range.isValid()) {
        return false;
    }
    bool changed = false;
    QSet<quint64> visited;
    for (int row = range.top; row <= range.bottom; ++row) {
        for (int column = range.left; column <= range.right; ++column) {
            const QPoint anchor = anchorAt(row, column);
            const quint64 key = coordinateKey(anchor.y(), anchor.x());
            if (anchor.x() < 0 || visited.contains(key)) {
                continue;
            }
            visited.insert(key);
            ScreenshotTableCell& cell = m_slots[anchor.y()][anchor.x()].cell;
            if (!cell.text.isEmpty()) {
                cell.text.clear();
                changed = true;
            }
        }
    }
    return changed;
}

ScreenshotTableDocument ScreenshotTableDocument::fromClipboardMimeData(const QMimeData& source) {
    ScreenshotTableDocument result;
    if (source.hasFormat(tableClipboardFormat)) {
        result = fromHtml(QString::fromUtf8(source.data(tableClipboardFormat)));
    }
    if (result.empty() && source.hasHtml()) {
        result = fromHtml(source.html());
    }
    if (result.empty() && source.hasText()) {
        result = fromPlainText(source.text());
    }
    return result;
}

std::unique_ptr<QMimeData> ScreenshotTableDocument::toClipboardMimeData() const {
    return toClipboardMimeData({0, 0, rowCount() - 1, columnCount() - 1});
}

std::unique_ptr<QMimeData>
ScreenshotTableDocument::toClipboardMimeData(const ScreenshotTableRange& source) const {
    const ScreenshotTableRange range = expandedRange(source);
    if (!range.isValid()) {
        return {};
    }
    auto mimeData = std::make_unique<QMimeData>();
    // Keep an unmodified representation for copying back into the table editor.
    mimeData->setData(tableClipboardFormat, toHtml(range).toUtf8());
    ScreenshotTableDocument spreadsheet = *this;
    for (int row = range.top; row <= range.bottom; ++row) {
        for (int column = range.left; column <= range.right; ++column) {
            if (spreadsheet.isAnchor(row, column)) {
                QString& text = spreadsheet.m_slots[row][column].cell.text;
                if (needsSpreadsheetTextPrefix(text)) {
                    text.prepend(QLatin1Char('\''));
                }
            }
        }
    }
    // Spreadsheet HTML importers need document context around the table. A bare
    // fragment can make the Windows CF_HTML transport header appear as cell text.
    // Qt supplies that native header and calculates its UTF-8 byte offsets.
    mimeData->setHtml(QStringLiteral("<html><head><meta charset=\"utf-8\"></head><body>"
                                     "<!--StartFragment-->") +
                      spreadsheet.toHtml(range) +
                      QStringLiteral("<!--EndFragment--></body></html>"));
    mimeData->setText(spreadsheet.toPlainText(range));
    return mimeData;
}

QByteArray ScreenshotTableDocument::contentCheckpoint() const {
    if (m_contentCheckpoint.isEmpty())
        m_contentCheckpoint =
            QCryptographicHash::hash(toHtml().toUtf8(), QCryptographicHash::Sha256);
    return m_contentCheckpoint;
}

QString ScreenshotTableDocument::toHtml() const {
    return toHtml(ScreenshotTableRange{0, 0, rowCount() - 1, columnCount() - 1});
}

QString ScreenshotTableDocument::toHtml(const ScreenshotTableRange& source) const {
    const ScreenshotTableRange range = expandedRange(source);
    if (!range.isValid()) {
        return {};
    }

    QString output = QStringLiteral("<table style=\"border-collapse:collapse;\"><tbody>");
    for (int row = range.top; row <= range.bottom; ++row) {
        output += QStringLiteral("<tr>");
        for (int column = range.left; column <= range.right; ++column) {
            if (!isAnchor(row, column)) {
                continue;
            }
            const ScreenshotTableCell& cell = m_slots.at(row).at(column).cell;
            const QString tag = cell.header ? QStringLiteral("th") : QStringLiteral("td");
            output += QLatin1Char('<') + tag;
            if (cell.rowSpan > 1) {
                output += QStringLiteral(" rowspan=\"%1\"").arg(cell.rowSpan);
            }
            if (cell.columnSpan > 1) {
                output += QStringLiteral(" colspan=\"%1\"").arg(cell.columnSpan);
            }
            output += QLatin1Char('>') + escapedCellText(cell.text) + QStringLiteral("</") + tag +
                      QLatin1Char('>');
        }
        output += QStringLiteral("</tr>");
    }
    output += QStringLiteral("</tbody></table>");
    return output;
}

QString ScreenshotTableDocument::toPlainText() const {
    return toPlainText(ScreenshotTableRange{0, 0, rowCount() - 1, columnCount() - 1});
}

QString ScreenshotTableDocument::toPlainText(const ScreenshotTableRange& source) const {
    const ScreenshotTableRange range = expandedRange(source);
    if (!range.isValid()) {
        return {};
    }
    QString output;
    for (int row = range.top; row <= range.bottom; ++row) {
        if (row > range.top) {
            output += QLatin1Char('\n');
        }
        for (int column = range.left; column <= range.right; ++column) {
            if (column > range.left) {
                output += QLatin1Char('\t');
            }
            if (isAnchor(row, column)) {
                output += m_slots.at(row).at(column).cell.text;
            }
        }
    }
    return output;
}

bool ScreenshotTableDocument::operator==(const ScreenshotTableDocument& other) const {
    return m_slots == other.m_slots;
}

bool ScreenshotTableDocument::contains(int row, int column) const {
    return row >= 0 && row < rowCount() && column >= 0 && column < columnCount();
}

ScreenshotTableRange
ScreenshotTableDocument::boundedRange(const ScreenshotTableRange& range) const {
    if (!range.isValid() || empty()) {
        return {};
    }
    ScreenshotTableRange bounded{
        std::clamp(range.top, 0, rowCount() - 1),
        std::clamp(range.left, 0, columnCount() - 1),
        std::clamp(range.bottom, 0, rowCount() - 1),
        std::clamp(range.right, 0, columnCount() - 1),
    };
    return bounded.isValid() ? bounded : ScreenshotTableRange{};
}

void ScreenshotTableDocument::initializeSlots(int rows, int columns, bool firstRowHeader) {
    if (rows <= 0 || columns <= 0) {
        return;
    }
    m_slots.resize(rows);
    for (int row = 0; row < rows; ++row) {
        m_slots[row].resize(columns);
        for (int column = 0; column < columns; ++column) {
            Slot& slot = m_slots[row][column];
            slot.anchorRow = row;
            slot.anchorColumn = column;
            slot.cell.header = firstRowHeader && row == 0;
        }
    }
}

void ScreenshotTableDocument::applySpan(int row, int column) {
    if (!isAnchor(row, column)) {
        return;
    }
    Slot& anchor = m_slots[row][column];
    anchor.cell.rowSpan = std::clamp(anchor.cell.rowSpan, 1, rowCount() - row);
    anchor.cell.columnSpan = std::clamp(anchor.cell.columnSpan, 1, columnCount() - column);
    for (int coveredRow = row; coveredRow < row + anchor.cell.rowSpan; ++coveredRow) {
        for (int coveredColumn = column; coveredColumn < column + anchor.cell.columnSpan;
             ++coveredColumn) {
            if (coveredRow == row && coveredColumn == column) {
                continue;
            }
            Slot& covered = m_slots[coveredRow][coveredColumn];
            covered.cell = ScreenshotTableCell{};
            covered.anchorRow = row;
            covered.anchorColumn = column;
        }
    }
}

void ScreenshotTableDocument::splitAnchor(int row, int column, bool headerRow) {
    if (!isAnchor(row, column)) {
        return;
    }
    const ScreenshotTableCell merged = m_slots.at(row).at(column).cell;
    for (int splitRow = row; splitRow < row + merged.rowSpan; ++splitRow) {
        for (int splitColumn = column; splitColumn < column + merged.columnSpan; ++splitColumn) {
            Slot& slot = m_slots[splitRow][splitColumn];
            slot = Slot{};
            slot.anchorRow = splitRow;
            slot.anchorColumn = splitColumn;
            slot.cell.header = headerRow && splitRow == 0;
        }
    }
    m_slots[row][column].cell.text = merged.text;
}
