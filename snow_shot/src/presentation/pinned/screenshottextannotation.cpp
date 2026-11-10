#include "snow_shot/presentation/screenshottextannotation.h"

#include <QDataStream>
#include <QIODevice>
#include <QMap>
#include <algorithm>
#include <cmath>

namespace {
using Range = ScreenshotTextSelectionRange;
using Run = ScreenshotTextAnnotationRun;
using Style = ScreenshotTextAnnotationStyle;
using Tool = ScreenshotTextAnnotationTool;
constexpr qsizetype kMaximumPayload = 32 * 1024 * 1024;

bool validStyle(const Style& style) {
    return style.color.isValid() && std::isfinite(style.width) && style.width > 0 &&
           style.width <= 72 && static_cast<unsigned>(style.strokeStyle) <= 2 &&
           static_cast<unsigned>(style.renderingStyle) <= 2;
}

QVector<Range> normalized(QVector<Range> ranges) {
    ranges.removeIf(
        [](const Range& r) { return r.sourceLine < 0 || r.start < 0 || r.end <= r.start; });
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) {
        return a.sourceLine < b.sourceLine || (a.sourceLine == b.sourceLine && a.start < b.start);
    });
    QVector<Range> result;
    for (const auto& range : ranges) {
        if (!result.empty() && result.back().sourceLine == range.sourceLine &&
            range.start <= result.back().end) {
            result.back().end = std::max(range.end, result.back().end);
        } else {
            result.push_back(range);
        }
    }
    return result;
}

const Style* styleFor(const Run& run, Tool tool) {
    if (tool == Tool::Highlight)
        return run.highlight ? &*run.highlight : nullptr;
    return run.line && run.line->tool == tool ? &run.line->style : nullptr;
}

void append(QVector<Run>& result, Run run) {
    if (!run.highlight && !run.line)
        return;
    if (!result.empty() && result.back().range.sourceLine == run.range.sourceLine &&
        result.back().range.end == run.range.start && result.back().highlight == run.highlight &&
        result.back().line == run.line) {
        result.back().range.end = run.range.end;
    } else {
        result.push_back(std::move(run));
    }
}

void writeStyle(QDataStream& stream, const Style& style) {
    stream << quint32(style.color.rgba()) << double(style.width) << quint8(style.strokeStyle)
           << quint8(style.renderingStyle);
}

Style readStyle(QDataStream& stream) {
    quint32 rgba = 0;
    double width = 0;
    quint8 pattern = 0, rendering = 0;
    stream >> rgba >> width >> pattern >> rendering;
    return {QColor::fromRgba(rgba), width, static_cast<SnowCanvasStrokeStyle>(pattern),
            static_cast<SnowCanvasStrokeRenderingStyle>(rendering)};
}
} // namespace

void ScreenshotTextAnnotationDocument::bindSource(ScreenshotTextAnnotationSource source,
                                                  const QByteArray& fingerprint) {
    if (m_source == source && m_fingerprint == fingerprint)
        return;
    m_source = source;
    m_fingerprint = fingerprint;
    m_runs.clear();
    ++m_revision;
}

void ScreenshotTextAnnotationDocument::clear() {
    if (m_runs.empty())
        return;
    m_runs.clear();
    ++m_revision;
}

bool ScreenshotTextAnnotationDocument::validForSource(const QVector<int>& sourceLengths) const {
    for (const auto& run : m_runs) {
        if (run.range.sourceLine >= sourceLengths.size() ||
            run.range.end > sourceLengths[run.range.sourceLine])
            return false;
    }
    return true;
}

bool ScreenshotTextAnnotationDocument::hasStyle(const QVector<Range>& selection, Tool tool,
                                                bool requireFull) const {
    const auto ranges = normalized(selection);
    if (ranges.empty())
        return false;
    qsizetype index = 0;
    for (const auto& range : ranges) {
        while (index < m_runs.size() && (m_runs[index].range.sourceLine < range.sourceLine ||
                                         (m_runs[index].range.sourceLine == range.sourceLine &&
                                          m_runs[index].range.end <= range.start)))
            ++index;
        int covered = range.start;
        for (auto i = index; i < m_runs.size(); ++i) {
            const auto& run = m_runs[i];
            if (run.range.sourceLine != range.sourceLine || run.range.start >= range.end)
                break;
            if (styleFor(run, tool)) {
                if (!requireFull)
                    return true;
                if (run.range.start > covered)
                    return false;
                covered = std::max(covered, run.range.end);
            }
        }
        if (requireFull && covered < range.end)
            return false;
    }
    return requireFull;
}

std::optional<Style>
ScreenshotTextAnnotationDocument::selectedStyle(const QVector<Range>& selection, Tool tool,
                                                bool* mixed, quint32* mixedProperties) const {
    if (mixed)
        *mixed = false;
    if (mixedProperties)
        *mixedProperties = 0;
    std::optional<Style> result;
    const auto ranges = normalized(selection);
    qsizetype index = 0;
    for (const auto& range : ranges) {
        while (index < m_runs.size() && (m_runs[index].range.sourceLine < range.sourceLine ||
                                         (m_runs[index].range.sourceLine == range.sourceLine &&
                                          m_runs[index].range.end <= range.start)))
            ++index;
        for (auto i = index; i < m_runs.size(); ++i) {
            const auto& run = m_runs[i];
            if (run.range.sourceLine != range.sourceLine || run.range.start >= range.end)
                break;
            if (const auto* style = styleFor(run, tool)) {
                if (result && *result != *style && mixed)
                    *mixed = true;
                if (result && mixedProperties) {
                    if (result->color != style->color)
                        *mixedProperties |= ScreenshotTextAnnotationStylePropertyColor;
                    if (result->width != style->width)
                        *mixedProperties |= ScreenshotTextAnnotationStylePropertyWidth;
                    if (result->strokeStyle != style->strokeStyle)
                        *mixedProperties |= ScreenshotTextAnnotationStylePropertyStrokeStyle;
                    if (result->renderingStyle != style->renderingStyle)
                        *mixedProperties |= ScreenshotTextAnnotationStylePropertyRenderingStyle;
                }
                if (!result)
                    result = *style;
            }
        }
    }
    return result;
}

bool ScreenshotTextAnnotationDocument::toggle(const QVector<Range>& selection, Tool tool,
                                              const Style& style) {
    return mutate(selection, tool, style, hasStyle(selection, tool), false);
}

bool ScreenshotTextAnnotationDocument::updateStyle(const QVector<Range>& selection, Tool tool,
                                                   const Style& style, quint32 properties) {
    return mutate(selection, tool, style, false, true, properties);
}

bool ScreenshotTextAnnotationDocument::mutate(const QVector<Range>& selection, Tool tool,
                                              const Style& style, bool remove, bool existingOnly,
                                              quint32 properties) {
    if (static_cast<unsigned>(tool) > 3 || !validStyle(style))
        return false;
    const auto ranges = normalized(selection);
    if (ranges.empty())
        return false;
    QMap<int, QVector<int>> boundaries;
    for (const auto& run : m_runs)
        boundaries[run.range.sourceLine] << run.range.start << run.range.end;
    for (const auto& range : ranges)
        boundaries[range.sourceLine] << range.start << range.end;
    QVector<Run> result;
    qsizetype runIndex = 0, rangeIndex = 0;
    for (auto it = boundaries.begin(); it != boundaries.end(); ++it) {
        auto& positions = it.value();
        std::sort(positions.begin(), positions.end());
        positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
        for (qsizetype i = 1; i < positions.size(); ++i) {
            const Range piece{it.key(), positions[i - 1], positions[i]};
            while (runIndex < m_runs.size() &&
                   (m_runs[runIndex].range.sourceLine < piece.sourceLine ||
                    (m_runs[runIndex].range.sourceLine == piece.sourceLine &&
                     m_runs[runIndex].range.end <= piece.start)))
                ++runIndex;
            while (rangeIndex < ranges.size() &&
                   (ranges[rangeIndex].sourceLine < piece.sourceLine ||
                    (ranges[rangeIndex].sourceLine == piece.sourceLine &&
                     ranges[rangeIndex].end <= piece.start)))
                ++rangeIndex;
            Run run{piece, {}, {}};
            if (runIndex < m_runs.size() && m_runs[runIndex].range.sourceLine == piece.sourceLine &&
                m_runs[runIndex].range.start <= piece.start) {
                run.highlight = m_runs[runIndex].highlight;
                run.line = m_runs[runIndex].line;
            }
            const bool selected = rangeIndex < ranges.size() &&
                                  ranges[rangeIndex].sourceLine == piece.sourceLine &&
                                  ranges[rangeIndex].start <= piece.start;
            if (selected && (!existingOnly || styleFor(run, tool))) {
                Style applied = style;
                if (existingOnly) {
                    applied = *styleFor(run, tool);
                    if (properties & ScreenshotTextAnnotationStylePropertyColor)
                        applied.color = style.color;
                    if (properties & ScreenshotTextAnnotationStylePropertyWidth)
                        applied.width = style.width;
                    if (properties & ScreenshotTextAnnotationStylePropertyStrokeStyle)
                        applied.strokeStyle = style.strokeStyle;
                    if (properties & ScreenshotTextAnnotationStylePropertyRenderingStyle)
                        applied.renderingStyle = style.renderingStyle;
                }
                if (tool == Tool::Highlight)
                    run.highlight = remove ? std::nullopt : std::optional<Style>(applied);
                else if (remove)
                    run.line.reset();
                else
                    run.line = ScreenshotTextAnnotationLine{tool, applied};
            }
            append(result, std::move(run));
        }
    }
    if (result == m_runs)
        return false;
    m_runs = std::move(result);
    ++m_revision;
    return true;
}

QByteArray ScreenshotTextAnnotationDocument::serialize() const {
    if (empty())
        return {};
    QByteArray payload;
    QDataStream stream(&payload, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_11);
    stream << quint32(0x53544131) << quint8(1) << quint8(m_source) << quint32(m_fingerprint.size());
    stream.writeRawData(m_fingerprint.constData(), m_fingerprint.size());
    stream << quint32(m_runs.size());
    for (const auto& run : m_runs) {
        stream << qint32(run.range.sourceLine) << qint32(run.range.start) << qint32(run.range.end)
               << quint8((run.highlight ? 1 : 0) | (run.line ? 2 : 0));
        if (run.highlight)
            writeStyle(stream, *run.highlight);
        if (run.line) {
            stream << quint8(run.line->tool);
            writeStyle(stream, run.line->style);
        }
    }
    return payload.size() <= kMaximumPayload ? payload : QByteArray{};
}

bool ScreenshotTextAnnotationDocument::restore(const QByteArray& payload) {
    if (payload.isEmpty()) {
        clear();
        return true;
    }
    if (payload.size() > kMaximumPayload)
        return false;
    QDataStream stream(payload);
    stream.setVersion(QDataStream::Qt_6_11);
    quint32 magic = 0, fingerprintSize = 0, count = 0;
    quint8 version = 0, source = 0;
    stream >> magic >> version >> source >> fingerprintSize;
    if (magic != 0x53544131 || version != 1 || source > 1 || fingerprintSize > 128 ||
        fingerprintSize > stream.device()->bytesAvailable())
        return false;
    QByteArray fingerprint(fingerprintSize, Qt::Uninitialized);
    if (stream.readRawData(fingerprint.data(), fingerprintSize) != fingerprintSize)
        return false;
    stream >> count;
    if (count > quint64(stream.device()->bytesAvailable() / 27))
        return false;
    QVector<Run> runs;
    runs.reserve(count);
    for (quint32 i = 0; i < count; ++i) {
        qint32 line = 0, start = 0, end = 0;
        quint8 flags = 0;
        stream >> line >> start >> end >> flags;
        if (line < 0 || start < 0 || end <= start || flags == 0 || flags > 3 ||
            (source == 1 && line != 0))
            return false;
        Run run{{line, start, end}, {}, {}};
        if (flags & 1) {
            run.highlight = readStyle(stream);
            if (!validStyle(*run.highlight))
                return false;
        }
        if (flags & 2) {
            quint8 tool = 0;
            stream >> tool;
            run.line = ScreenshotTextAnnotationLine{static_cast<Tool>(tool), readStyle(stream)};
            if (tool < 1 || tool > 3 || !validStyle(run.line->style))
                return false;
        }
        if (!runs.empty() &&
            (runs.back().range.sourceLine > line ||
             (runs.back().range.sourceLine == line && runs.back().range.end > start)))
            return false;
        append(runs, std::move(run));
    }
    if (stream.status() != QDataStream::Ok || !stream.atEnd())
        return false;
    m_source = static_cast<ScreenshotTextAnnotationSource>(source);
    m_fingerprint = std::move(fingerprint);
    m_runs = std::move(runs);
    ++m_revision;
    return true;
}
