// Adapted from STranslate OcrLayoutAnalyzer.cs, Copyright (c) 2022 zggsong.
// MIT license: see snow_shot/THIRD_PARTY_NOTICES.md.
#include "snow_shot/presentation/screenshotocrlayout.h"

#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>

namespace snow_shot::presentation {
namespace {
struct Segment {
    ScreenshotOcrLine line;
    QRectF bounds;
    int peers = 1;
};
struct Group {
    QVector<Segment> lines;
    QRectF bounds;
    void add(const Segment& line) {
        bounds = lines.isEmpty() ? line.bounds : bounds.united(line.bounds);
        lines.push_back(line);
    }
};
qreal median(QVector<qreal> values) {
    values.erase(
        std::remove_if(values.begin(), values.end(), [](qreal value) { return value <= 0; }),
        values.end());
    if (values.isEmpty())
        return 0;
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}
qreal verticalOverlap(const QRectF& a, const QRectF& b) {
    return std::max<qreal>(0, std::min(a.bottom(), b.bottom()) - std::max(a.top(), b.top())) /
           std::max<qreal>(1, std::min(a.height(), b.height()));
}
qreal horizontalOverlap(const QRectF& a, const QRectF& b) {
    return std::max<qreal>(0, std::min(a.right(), b.right()) - std::max(a.left(), b.left())) /
           std::max<qreal>(1, std::min(a.width(), b.width()));
}
qreal gap(const QRectF& a, const QRectF& b) {
    return std::max<qreal>(0, b.top() - a.bottom());
}
bool topLeft(const Segment& a, const Segment& b) {
    return a.bounds.top() != b.bounds.top() ? a.bounds.top() < b.bounds.top()
                                            : a.bounds.left() < b.bounds.left();
}
bool sameRow(const QRectF& a, const QRectF& b, qreal height = 0) {
    return height > 0
               ? verticalOverlap(a, b) >= 0.35 ||
                     std::abs(a.center().y() - b.center().y()) <= height * 0.55
               : verticalOverlap(a, b) >= 0.55 || std::abs(a.center().y() - b.center().y()) <=
                                                      std::min(a.height(), b.height()) * 0.45;
}
QPolygonF quad(const QRectF& bounds) {
    return {bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()};
}
bool latin(QChar c) {
    return (c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
           (c >= QLatin1Char('A') && c <= QLatin1Char('Z'));
}
bool cjk(QChar c) {
    const auto n = c.unicode();
    return (n >= 0x3400 && n <= 0x9fff) || (n >= 0x3040 && n <= 0x30ff) ||
           (n >= 0xac00 && n <= 0xd7af) || (n >= 0xf900 && n <= 0xfaff);
}
bool hyphenated(const QString& a, const QString& b) {
    return a.size() >= 2 && !b.isEmpty() && a.back() == QLatin1Char('-') &&
           latin(a[a.size() - 2]) && latin(b.front()) && b.front().isLower();
}
QString join(QString a, const QString& b, bool paragraph) {
    if (a.isEmpty())
        return b;
    if (b.isEmpty())
        return a;
    if (paragraph && hyphenated(a, b)) {
        a.chop(1);
    } else if (!a.back().isSpace() && !b.front().isSpace() && !cjk(a.back()) && !cjk(b.front()) &&
               !a.back().isPunct() && !b.front().isPunct()) {
        a += QLatin1Char(' ');
    }
    return a + b;
}
int listNumber(const QString& text) {
    static const QRegularExpression pattern(QStringLiteral("^\\s*(\\d+)[.)\\x{3001}]\\s"));
    const auto match = pattern.match(text);
    bool ok = false;
    const int result = match.captured(1).toInt(&ok);
    return match.hasMatch() && ok ? result : -1;
}
bool listStart(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;
    const QString bullets = QStringLiteral("-*+\u2022\u00b7\u25cf\u25aa");
    return (bullets.contains(trimmed.front()) && (trimmed.size() == 1 || trimmed[1].isSpace())) ||
           listNumber(trimmed) >= 0;
}
bool sentenceChar(QChar c) {
    return QStringLiteral(".!?;:\u3002\uff01\uff1f\uff1b\uff1a").contains(c);
}
bool hasSentenceEnding(const QString& text) {
    return std::any_of(text.begin(), text.end(), sentenceChar);
}
bool endsSentence(const QString& text) {
    for (auto i = text.size(); i > 0;) {
        const auto c = text[--i];
        if (!c.isSpace() && !QStringLiteral("\"')]}\u201d\u2019").contains(c))
            return sentenceChar(c);
    }
    return false;
}
bool startsUpper(const QString& text) {
    for (const auto c : text.trimmed()) {
        if (!QStringLiteral("\"'([{\u201c\u2018").contains(c))
            return c >= QLatin1Char('A') && c <= QLatin1Char('Z');
    }
    return false;
}
bool standalone(const Segment& line) {
    const auto& text = line.line.text;
    return !listStart(text) && text.size() <= 14 &&
           std::none_of(text.begin(), text.end(), [](QChar c) { return c.isSpace(); }) &&
           line.bounds.width() / std::max<qreal>(line.bounds.height(), 1) <= 6.2 &&
           !hasSentenceEnding(text);
}
int wordCount(const QString& text) {
    return static_cast<int>(text.split(QLatin1Char(' '), Qt::SkipEmptyParts).size());
}
bool gridCell(const Segment& line, qreal height) {
    return line.peers > 1 && !listStart(line.line.text) && !hasSentenceEnding(line.line.text) &&
           wordCount(line.line.text) <= 3 && line.line.text.size() <= 32 &&
           line.bounds.width() <= height * 7;
}
bool tableItem(const Segment& line, qreal height, int columns) {
    return line.peers > 1 && !listStart(line.line.text) && !hasSentenceEnding(line.line.text) &&
           wordCount(line.line.text) <= (columns >= 3 ? 6 : 3) &&
           line.line.text.size() <= (columns >= 3 ? 72 : 48) &&
           line.bounds.width() / std::max<qreal>(height, 1) <= (columns >= 3 ? 18 : 12);
}
struct Cluster {
    qreal center;
    int count;
};
QVector<Cluster> clusters(QVector<qreal> positions, qreal tolerance) {
    std::sort(positions.begin(), positions.end());
    QVector<Cluster> result;
    for (const auto position : positions) {
        int best = -1;
        qreal distance = std::numeric_limits<qreal>::max();
        for (int i = 0; i < result.size(); ++i) {
            const auto delta = std::abs(result[i].center - position);
            if (delta <= tolerance && delta < distance) {
                best = i;
                distance = delta;
            }
        }
        if (best < 0)
            result.push_back({position, 1});
        else {
            auto& cluster = result[best];
            cluster.center = (cluster.center * cluster.count + position) / (cluster.count + 1);
            ++cluster.count;
        }
    }
    return result;
}
QVector<Group> rows(QVector<Segment> lines, qreal height = 0) {
    std::stable_sort(lines.begin(), lines.end(), [](const auto& a, const auto& b) {
        return a.bounds.center().y() != b.bounds.center().y()
                   ? a.bounds.center().y() < b.bounds.center().y()
                   : a.bounds.left() < b.bounds.left();
    });
    QVector<Group> result;
    for (const auto& line : lines) {
        int best = -1;
        qreal overlap = -1, delta = std::numeric_limits<qreal>::max();
        for (int i = 0; i < result.size(); ++i) {
            const auto& bounds = result[i].bounds;
            const qreal candidateOverlap = verticalOverlap(bounds, line.bounds);
            const qreal candidateDelta = std::abs(bounds.center().y() - line.bounds.center().y());
            if (sameRow(bounds, line.bounds, height) &&
                (candidateOverlap > overlap ||
                 (candidateOverlap == overlap && candidateDelta < delta))) {
                best = i;
                overlap = candidateOverlap;
                delta = candidateDelta;
            }
        }
        if (best < 0) {
            result.push_back({});
            best = static_cast<int>(result.size() - 1);
        }
        result[best].add(line);
    }
    return result;
}
qreal rowGap(QVector<Group> groups) {
    std::stable_sort(groups.begin(), groups.end(),
                     [](const auto& a, const auto& b) { return a.bounds.top() < b.bounds.top(); });
    QVector<qreal> gaps;
    for (int i = 1; i < groups.size(); ++i)
        gaps.push_back(gap(groups[i - 1].bounds, groups[i].bounds));
    return median(gaps);
}
QVector<Segment> splitNumberedList(const Segment& item) {
    QString text = item.line.text;
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"))
        .replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const auto lines = text.split(QLatin1Char('\n'));
    QVector<int> starts;
    int previous = -1, firstText = -1;
    for (int i = 0; i < lines.size(); ++i) {
        if (firstText < 0 && !lines[i].trimmed().isEmpty())
            firstText = i;
        const int number = listNumber(lines[i]);
        if (number < 0)
            continue;
        if (previous >= 0 && number != previous + 1)
            return {item};
        previous = number;
        starts.push_back(i);
    }
    if (starts.size() < 2 || starts.front() != firstText)
        return {item};
    QVector<Segment> result;
    for (int i = 0; i < starts.size(); ++i) {
        const int start = starts[i],
                  end = i + 1 < starts.size() ? starts[i + 1] : static_cast<int>(lines.size());
        Segment part = item;
        part.line.text.clear();
        for (int j = start; j < end; ++j)
            part.line.text = join(part.line.text, lines[j].trimmed(), true);
        part.bounds = QRectF(
            item.bounds.left(), item.bounds.top() + item.bounds.height() * start / lines.size(),
            item.bounds.width(), item.bounds.height() * (end - start) / lines.size());
        part.line.quad = quad(part.bounds);
        result.push_back(part);
    }
    return result;
}
QVector<Segment> lineSegments(const QVector<Segment>& items) {
    auto visualRows = rows(items);
    QVector<qreal> heights, positions;
    int peerRows = 0;
    for (const auto& row : visualRows) {
        if (row.lines.size() >= 2)
            ++peerRows;
        for (const auto& line : row.lines) {
            heights.push_back(line.bounds.height());
            if (row.lines.size() >= 2)
                positions.push_back(line.bounds.left());
        }
    }
    const auto height = std::max<qreal>(1, median(heights));
    auto columns = clusters(positions, std::max<qreal>(1, height * 1.4));
    columns.erase(
        std::remove_if(columns.begin(), columns.end(), [](const auto& c) { return c.count < 3; }),
        columns.end());
    const bool table = peerRows >= 3 && rowGap(visualRows) >= height * 0.32 &&
                       columns.size() >= 2 &&
                       columns.back().center - columns.front().center >= height * 6;
    QVector<Segment> result;
    for (auto& row : visualRows) {
        std::stable_sort(row.lines.begin(), row.lines.end(), [](const auto& a, const auto& b) {
            return a.bounds.left() < b.bounds.left();
        });
        QVector<qreal> rowHeights;
        for (const auto& item : row.lines)
            rowHeights.push_back(item.bounds.height());
        const auto h = median(rowHeights);
        QVector<Group> groups;
        for (const auto& item : row.lines) {
            bool split = groups.isEmpty();
            if (!split) {
                const auto& group = groups.back();
                const auto& previous = group.lines.back();
                const auto maxGap = std::max(
                    h * 1.25,
                    std::min(h * 2, std::min(previous.bounds.width(), item.bounds.width()) * 0.75));
                const bool adornment =
                    group.bounds.width() <= h * 1.25 && group.bounds.height() <= h * 1.35;
                const bool columnStart =
                    std::any_of(columns.begin(), columns.end(), [&](const auto& c) {
                        return std::abs(c.center - item.bounds.left()) <=
                               std::max<qreal>(1, h * 1.4);
                    });
                split = item.bounds.left() - previous.bounds.right() > maxGap ||
                        (table && !adornment &&
                         item.bounds.left() - group.bounds.right() >= h * 0.75 && columnStart);
            }
            if (split)
                groups.push_back({});
            groups.back().add(item);
        }
        for (const auto& group : groups) {
            Segment segment = group.lines.front();
            segment.line.sourceLineQuads.clear();
            for (const auto& source : group.lines)
                segment.line.sourceLineQuads.push_back(source.line.quad);
            segment.bounds = group.bounds;
            segment.peers = static_cast<int>(groups.size());
            for (int i = 1; i < group.lines.size(); ++i)
                segment.line.text = join(segment.line.text, group.lines[i].line.text, false);
            if (group.lines.size() > 1)
                segment.line.quad = quad(group.bounds);
            result.push_back(segment);
        }
    }
    std::stable_sort(result.begin(), result.end(), topLeft);
    return result;
}
qreal normalGap(QVector<Segment> lines, qreal height) {
    std::stable_sort(lines.begin(), lines.end(), topLeft);
    QVector<qreal> gaps, normal;
    for (int i = 1; i < lines.size(); ++i) {
        if (verticalOverlap(lines[i - 1].bounds, lines[i].bounds) > 0.25)
            continue;
        const auto value = gap(lines[i - 1].bounds, lines[i].bounds);
        gaps.push_back(value);
        if (value <= height * 0.65)
            normal.push_back(value);
    }
    if (!normal.isEmpty())
        return median(normal);
    std::sort(gaps.begin(), gaps.end());
    if (!gaps.isEmpty())
        gaps.resize(std::max<qsizetype>(1, gaps.size() / 2));
    return median(gaps);
}
QVector<Group> regions(const QVector<Segment>& lines, qreal height) {
    QVector<Group> result;
    for (const auto& line : lines) {
        int best = -1;
        qreal bestScore = -1;
        for (int i = 0; i < result.size(); ++i) {
            const auto& references = result[i].lines;
            for (qsizetype j = std::max<qsizetype>(0, references.size() - 4); j < references.size();
                 ++j) {
                const auto& reference = references[j].bounds;
                const auto& bounds = line.bounds;
                const auto distance = gap(reference, bounds);
                if (bounds.top() < reference.top() - height * 0.35 || distance > height * 3.2)
                    continue;
                const auto overlap = horizontalOverlap(reference, bounds);
                const auto left = std::abs(reference.left() - bounds.left());
                const auto center = std::abs(reference.center().x() - bounds.center().x());
                const auto width = std::max(reference.width(), bounds.width());
                if (overlap < 0.30 && left > height * 1.8 && center > width * 0.35)
                    continue;
                const auto score =
                    overlap * 4 +
                    (1 - std::min<qreal>(1, left / std::max<qreal>(height * 2, 1))) * 2 + 1 -
                    std::min<qreal>(1, center / std::max<qreal>(width, 1)) + 1 -
                    std::min<qreal>(1, distance / std::max<qreal>(height * 3.2, 1)) + 1 -
                    std::min<qreal>(1, std::abs(reference.width() - bounds.width()) /
                                           std::max<qreal>(width, 1));
                if (score > bestScore) {
                    bestScore = score;
                    best = i;
                }
            }
        }
        if (best < 0) {
            result.push_back({});
            best = static_cast<int>(result.size() - 1);
        }
        result[best].add(line);
    }
    std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.bounds.top() != b.bounds.top() ? a.bounds.top() < b.bounds.top()
                                                : a.bounds.left() < b.bounds.left();
    });
    QVector<Group> ordered;
    while (!result.isEmpty()) {
        QVector<Group> band{result.takeFirst()};
        auto bounds = band.front().bounds;
        for (int i = 0; i < result.size();) {
            const auto candidate = result[i].bounds;
            if (verticalOverlap(bounds, candidate) >= 0.25 ||
                (std::abs(bounds.top() - candidate.top()) <= height * 2 &&
                 candidate.top() <= bounds.bottom() + height * 1.5)) {
                bounds = bounds.united(candidate);
                band.push_back(result.takeAt(i));
            } else
                ++i;
        }
        std::stable_sort(band.begin(), band.end(), [](const auto& a, const auto& b) {
            return a.bounds.left() != b.bounds.left() ? a.bounds.left() < b.bounds.left()
                                                      : a.bounds.top() < b.bounds.top();
        });
        ordered += band;
    }
    return ordered;
}
bool tableRegion(const Group& region, qreal height, int columns) {
    const auto tableRows = rows(region.lines, height);
    if (tableRows.size() < 3 || rowGap(tableRows) < height * 0.32 || columns < 2)
        return false;
    int peerRows = 0, itemRows = 0, count = 0;
    QVector<qreal> lefts, centers;
    for (const auto& row : tableRows) {
        bool peer = false, item = false;
        for (const auto& line : row.lines) {
            peer |= line.peers > 1;
            if (tableItem(line, height, columns)) {
                item = true;
                ++count;
                lefts.push_back(line.bounds.left());
                centers.push_back(line.bounds.center().x());
            }
        }
        peerRows += peer ? 1 : 0;
        itemRows += item ? 1 : 0;
    }
    if (count < 3 || peerRows < 3 || itemRows < 3)
        return false;
    const auto recurring = [height](const QVector<qreal>& positions) {
        const auto values = clusters(positions, std::max<qreal>(height * 1.4, 1));
        return std::any_of(values.begin(), values.end(),
                           [](const auto& c) { return c.count >= 3; });
    };
    return recurring(lefts) || recurring(centers);
}
qreal appendConfidence(const Group& paragraph, const Segment& current, qreal height, qreal normal) {
    const auto& previous = paragraph.lines.back();
    const auto& a = previous.bounds;
    const auto& b = current.bounds;
    const auto distance = gap(a, b);
    if (distance > height * 1.25 || listStart(current.line.text))
        return -1;
    if (listStart(previous.line.text) && b.left() > a.left() + height * 0.8)
        return 0.95;
    if (std::max(a.height(), b.height()) > std::min(a.height(), b.height()) * 1.45)
        return -1;
    const auto overlap = horizontalOverlap(a, b), left = std::abs(a.left() - b.left());
    if (overlap < 0.45 && left > height * 1.2)
        return -1;
    if (hyphenated(previous.line.text, current.line.text))
        return 0.95;
    QVector<qreal> bodyLefts, widths;
    for (int i = 0; i < paragraph.lines.size(); ++i) {
        widths.push_back(paragraph.lines[i].bounds.width());
        if (i > 0)
            bodyLefts.push_back(paragraph.lines[i].bounds.left());
    }
    const auto bodyLeft =
        bodyLefts.isEmpty() ? paragraph.lines.front().bounds.left() : median(bodyLefts);
    const auto typicalWidth = median(widths);
    const bool shortLine =
        a.width() <= b.width() * 0.82 ||
        (paragraph.lines.size() >= 2 && typicalWidth >= height * 6 &&
         a.width() <= typicalWidth * 0.72 && typicalWidth - a.width() >= height * 2.2);
    const auto indent = b.left() - bodyLeft;
    const bool firstIndent = indent >= height * 0.55 && indent <= height * 2.5;
    if (!listStart(previous.line.text) && !listStart(current.line.text)) {
        if (shortLine && a.left() <= bodyLeft + height * 0.45 && firstIndent && overlap >= 0.35)
            return -1;
        const auto threshold =
            std::max(height * 0.72, std::min(normal, height * 0.35) + height * 0.45);
        if (distance > threshold &&
            ((endsSentence(previous.line.text) &&
              (left <= height * 0.8 || b.left() > a.left() + height * 0.8)) ||
             (startsUpper(current.line.text) && (left <= height * 0.8 || firstIndent)) ||
             (shortLine && b.left() <= a.left() + height * 0.4 && overlap >= 0.45)))
            return -1;
    }
    if ((gridCell(previous, height) && gridCell(current, height)) || standalone(previous) ||
        standalone(current))
        return -1;
    if (left > height * 2.5 && overlap < 0.7)
        return -1;
    const auto leftAffinity = 1 - std::min<qreal>(1, left / std::max<qreal>(height * 1.2, 1));
    const auto gapAffinity = 1 - std::min<qreal>(1, distance / std::max<qreal>(height * 1.25, 1));
    const auto heightAffinity =
        std::min(a.height(), b.height()) / std::max<qreal>(std::max(a.height(), b.height()), 1);
    const auto confidence = std::clamp(std::max(overlap, leftAffinity) * 0.45 + gapAffinity * 0.35 +
                                           heightAffinity * 0.20,
                                       0.0, 1.0);
    return confidence >= 0.48 ? confidence : -1;
}
QVector<ScreenshotOcrLine> paragraphs(const Group& region, qreal height) {
    const auto normal = normalGap(region.lines, height);
    int columns = 1;
    for (const auto& line : region.lines)
        columns = std::max(columns, line.peers);
    const bool table = tableRegion(region, height, columns);
    QVector<Group> groups;
    for (const auto& line : region.lines) {
        int best = -1;
        qreal bestScore = -1;
        for (int i = 0; i < groups.size(); ++i) {
            const auto& previous = groups[i].lines.back();
            if (line.bounds.top() < previous.bounds.top() ||
                (table && tableItem(previous, height, columns) && tableItem(line, height, columns)))
                continue;
            const auto confidence = appendConfidence(groups[i], line, height, normal);
            if (confidence < 0)
                continue;
            const auto score =
                horizontalOverlap(previous.bounds, line.bounds) * 3 + 1 -
                std::min<qreal>(1, std::abs(previous.bounds.left() - line.bounds.left()) / height) +
                1 - std::min<qreal>(1, gap(previous.bounds, line.bounds) / height) + confidence;
            if (score > bestScore) {
                bestScore = score;
                best = i;
            }
        }
        if (best < 0) {
            groups.push_back({});
            best = static_cast<int>(groups.size() - 1);
        }
        groups[best].add(line);
    }
    std::stable_sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
        return a.bounds.top() != b.bounds.top() ? a.bounds.top() < b.bounds.top()
                                                : a.bounds.left() < b.bounds.left();
    });
    QVector<ScreenshotOcrLine> result;
    for (const auto& group : groups) {
        auto output = group.lines.front().line;
        output.paragraph = group.lines.size() > 1 || output.text.contains(QLatin1Char('\n'));
        output.sourceLineQuads.clear();
        for (int i = 0; i < group.lines.size(); ++i) {
            if (i > 0)
                output.text = join(output.text, group.lines[i].line.text, true);
            output.sourceLineQuads += group.lines[i].line.sourceLineQuads;
        }
        if (group.lines.size() > 1)
            output.quad = quad(group.bounds);
        result.push_back(output);
    }
    return result;
}
bool mergeable(const ScreenshotOcrLine& line) {
    if (line.direction != ScreenshotOcrTextDirection::Horizontal || line.text.trimmed().isEmpty() ||
        line.quad.size() != 4)
        return false;
    for (const auto& p : line.quad)
        if (!std::isfinite(p.x()) || !std::isfinite(p.y()))
            return false;
    const auto bounds = line.quad.boundingRect();
    // STranslate assumes horizontal rectangles. Preserve vertical/rotated OCR as original units.
    return bounds.width() > 0 && bounds.height() > 0 &&
           std::abs(line.quad[0].y() - line.quad[1].y()) <= bounds.height() * 0.1 &&
           std::abs(line.quad[2].y() - line.quad[3].y()) <= bounds.height() * 0.1;
}
} // namespace

QVector<ScreenshotOcrLine> mergeOcrLayout(const QVector<ScreenshotOcrLine>& lines,
                                          const QPointF& imageOrigin) {
    QVector<Segment> items;
    QVector<ScreenshotOcrLine> preserved;
    for (const auto& line : lines) {
        if (!mergeable(line)) {
            preserved.push_back(line);
            continue;
        }
        Segment item{line, line.quad.boundingRect()};
        // Upstream uses image coordinates; Snow Shot canvas coordinates can be negative.
        item.line.quad.translate(-imageOrigin);
        item.bounds.translate(-imageOrigin);
        item.line.text = item.line.text.trimmed();
        items += splitNumberedList(item);
    }
    const auto segments = lineSegments(items);
    QVector<qreal> heights;
    for (const auto& line : segments)
        heights.push_back(line.bounds.height());
    const auto height = std::max<qreal>(1, median(heights));
    QVector<ScreenshotOcrLine> result;
    for (const auto& region : regions(segments, height))
        result += paragraphs(region, height);
    for (auto& line : result) {
        line.quad.translate(imageOrigin);
        for (auto& sourceQuad : line.sourceLineQuads)
            sourceQuad.translate(imageOrigin);
    }
    result += preserved;
    return result;
}
} // namespace snow_shot::presentation
