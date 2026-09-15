#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTABLEDOCUMENT_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTABLEDOCUMENT_H

#include <QPoint>
#include <QString>
#include <QVector>
#include <memory>

class QMimeData;

struct ScreenshotTableRange {
    int top = -1;
    int left = -1;
    int bottom = -1;
    int right = -1;

    [[nodiscard]] bool isValid() const {
        return top >= 0 && left >= 0 && bottom >= top && right >= left;
    }
    [[nodiscard]] int rowCount() const {
        return isValid() ? bottom - top + 1 : 0;
    }
    [[nodiscard]] int columnCount() const {
        return isValid() ? right - left + 1 : 0;
    }

    [[nodiscard]] bool operator==(const ScreenshotTableRange& other) const {
        return top == other.top && left == other.left && bottom == other.bottom &&
               right == other.right;
    }
    [[nodiscard]] bool operator!=(const ScreenshotTableRange& other) const {
        return !(*this == other);
    }
};

struct ScreenshotTableCell {
    QString text;
    int rowSpan = 1;
    int columnSpan = 1;
    bool header = false;

    [[nodiscard]] bool operator==(const ScreenshotTableCell& other) const {
        return text == other.text && rowSpan == other.rowSpan && columnSpan == other.columnSpan &&
               header == other.header;
    }
    [[nodiscard]] bool operator!=(const ScreenshotTableCell& other) const {
        return !(*this == other);
    }
};

class ScreenshotTableDocument final {
  public:
    ScreenshotTableDocument() = default;
    ScreenshotTableDocument(int rows, int columns, bool firstRowIsHeader = false);

    [[nodiscard]] static ScreenshotTableDocument fromHtml(const QString& source);
    [[nodiscard]] static ScreenshotTableDocument fromPlainText(const QString& source);
    [[nodiscard]] static ScreenshotTableDocument fromClipboardMimeData(const QMimeData& source);

    [[nodiscard]] int rowCount() const;
    [[nodiscard]] int columnCount() const;
    [[nodiscard]] bool empty() const;
    [[nodiscard]] bool firstRowIsHeader() const;

    [[nodiscard]] QPoint anchorAt(int row, int column) const;
    [[nodiscard]] bool isAnchor(int row, int column) const;
    [[nodiscard]] const ScreenshotTableCell* anchorCellAt(int row, int column) const;
    [[nodiscard]] const ScreenshotTableCell* cellAt(int row, int column) const;
    [[nodiscard]] QString cellText(int row, int column) const;
    [[nodiscard]] ScreenshotTableRange spanRangeAt(int row, int column) const;
    [[nodiscard]] ScreenshotTableRange expandedRange(const ScreenshotTableRange& range) const;

    bool setCellText(int row, int column, const QString& text);
    [[nodiscard]] bool canMerge(const ScreenshotTableRange& range) const;
    bool merge(const ScreenshotTableRange& range);
    [[nodiscard]] bool canSplit(const ScreenshotTableRange& range) const;
    bool split(const ScreenshotTableRange& range);
    bool clear(const ScreenshotTableRange& range);

    [[nodiscard]] QString toHtml() const;
    [[nodiscard]] QString toHtml(const ScreenshotTableRange& range) const;
    [[nodiscard]] QString toPlainText() const;
    [[nodiscard]] QString toPlainText(const ScreenshotTableRange& range) const;
    [[nodiscard]] std::unique_ptr<QMimeData> toClipboardMimeData() const;
    [[nodiscard]] std::unique_ptr<QMimeData>
    toClipboardMimeData(const ScreenshotTableRange& range) const;

    [[nodiscard]] bool operator==(const ScreenshotTableDocument& other) const;
    [[nodiscard]] bool operator!=(const ScreenshotTableDocument& other) const {
        return !(*this == other);
    }

  private:
    struct Slot {
        ScreenshotTableCell cell;
        int anchorRow = -1;
        int anchorColumn = -1;

        [[nodiscard]] bool operator==(const Slot& other) const {
            return cell == other.cell && anchorRow == other.anchorRow &&
                   anchorColumn == other.anchorColumn;
        }
    };

    [[nodiscard]] bool contains(int row, int column) const;
    [[nodiscard]] ScreenshotTableRange boundedRange(const ScreenshotTableRange& range) const;
    void initializeSlots(int rows, int columns, bool firstRowIsHeader);
    void applySpan(int row, int column);
    void splitAnchor(int row, int column, bool headerRow);

    QVector<QVector<Slot>> m_slots;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTABLEDOCUMENT_H
