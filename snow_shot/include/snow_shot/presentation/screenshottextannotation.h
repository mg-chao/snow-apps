#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTEXTANNOTATION_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTEXTANNOTATION_H

#include "snow_draw_engine_qt/snow_canvas_types.h"
#include <QByteArray>
#include <QColor>
#include <QVector>
#include <optional>

enum class ScreenshotTextAnnotationSource { Ocr, Formatted };
enum class ScreenshotTextAnnotationTool { Highlight, WavyUnderline, Underline, Strikethrough };
enum ScreenshotTextAnnotationStyleProperty : quint32 {
    ScreenshotTextAnnotationStylePropertyColor = 1,
    ScreenshotTextAnnotationStylePropertyWidth = 2,
    ScreenshotTextAnnotationStylePropertyStrokeStyle = 4,
    ScreenshotTextAnnotationStylePropertyRenderingStyle = 8,
    ScreenshotTextAnnotationStylePropertyAll = 15,
};

struct ScreenshotTextSelectionRange final {
    int sourceLine = 0;
    int start = 0;
    int end = 0;
    friend bool operator==(const ScreenshotTextSelectionRange&,
                           const ScreenshotTextSelectionRange&) = default;
};

struct ScreenshotTextAnnotationStyle final {
    QColor color = QColor(245, 34, 45);
    qreal width = 2.0;
    SnowCanvasStrokeStyle strokeStyle = SnowCanvasStrokeStyle::Solid;
    SnowCanvasStrokeRenderingStyle renderingStyle = SnowCanvasStrokeRenderingStyle::Architect;
    friend bool operator==(const ScreenshotTextAnnotationStyle&,
                           const ScreenshotTextAnnotationStyle&) = default;
};

struct ScreenshotTextAnnotationLine final {
    ScreenshotTextAnnotationTool tool = ScreenshotTextAnnotationTool::Underline;
    ScreenshotTextAnnotationStyle style;
    friend bool operator==(const ScreenshotTextAnnotationLine&,
                           const ScreenshotTextAnnotationLine&) = default;
};

struct ScreenshotTextAnnotationRun final {
    ScreenshotTextSelectionRange range;
    std::optional<ScreenshotTextAnnotationStyle> highlight;
    std::optional<ScreenshotTextAnnotationLine> line;
    friend bool operator==(const ScreenshotTextAnnotationRun&,
                           const ScreenshotTextAnnotationRun&) = default;
};

// Logical ranges and applied values are independent of the selection surface and preferences.
class ScreenshotTextAnnotationDocument final {
  public:
    void bindSource(ScreenshotTextAnnotationSource source, const QByteArray& fingerprint);
    void clear();
    [[nodiscard]] bool empty() const {
        return m_runs.isEmpty();
    }
    [[nodiscard]] quint64 revision() const {
        return m_revision;
    }
    [[nodiscard]] ScreenshotTextAnnotationSource source() const {
        return m_source;
    }
    [[nodiscard]] const QByteArray& fingerprint() const {
        return m_fingerprint;
    }
    [[nodiscard]] const QVector<ScreenshotTextAnnotationRun>& runs() const {
        return m_runs;
    }
    [[nodiscard]] bool validForSource(const QVector<int>& sourceLengths) const;
    [[nodiscard]] bool hasStyle(const QVector<ScreenshotTextSelectionRange>& selection,
                                ScreenshotTextAnnotationTool tool, bool requireFull = true) const;
    [[nodiscard]] std::optional<ScreenshotTextAnnotationStyle>
    selectedStyle(const QVector<ScreenshotTextSelectionRange>& selection,
                  ScreenshotTextAnnotationTool tool, bool* mixed = nullptr,
                  quint32* mixedProperties = nullptr) const;
    bool toggle(const QVector<ScreenshotTextSelectionRange>& selection,
                ScreenshotTextAnnotationTool tool, const ScreenshotTextAnnotationStyle& style);
    bool updateStyle(const QVector<ScreenshotTextSelectionRange>& selection,
                     ScreenshotTextAnnotationTool tool, const ScreenshotTextAnnotationStyle& style,
                     quint32 properties = ScreenshotTextAnnotationStylePropertyAll);
    [[nodiscard]] QByteArray serialize() const;
    bool restore(const QByteArray& payload);

  private:
    bool mutate(const QVector<ScreenshotTextSelectionRange>& selection,
                ScreenshotTextAnnotationTool tool, const ScreenshotTextAnnotationStyle& style,
                bool remove, bool existingOnly,
                quint32 properties = ScreenshotTextAnnotationStylePropertyAll);
    ScreenshotTextAnnotationSource m_source = ScreenshotTextAnnotationSource::Ocr;
    QByteArray m_fingerprint;
    QVector<ScreenshotTextAnnotationRun> m_runs;
    quint64 m_revision = 0;
};

#endif
