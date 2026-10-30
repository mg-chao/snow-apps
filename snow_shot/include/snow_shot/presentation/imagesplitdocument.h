// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_IMAGESPLITDOCUMENT_H
#define SNOW_SHOT_PRESENTATION_IMAGESPLITDOCUMENT_H

#include <QImage>
#include <QMap>
#include <QObject>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>

namespace snow_shot::presentation {

enum class ImageSplitDirection { Vertical, Horizontal, Grid };
enum class ImageSplitType { Equal, Precise };
enum class ImageSplitUnit { Pixels, Percentage };

// Generated guides occupy arithmetic runs rather than one integer per pixel boundary. A persistent
// balanced tree shares untouched runs between manual edits and unlimited undo snapshots.
class ImageSplitLines final {
  public:
    class const_iterator final {
      public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = int;
        using difference_type = qsizetype;
        using pointer = void;
        using reference = int;

        const_iterator() = default;
        int operator*() const;
        int operator[](difference_type offset) const;
        const_iterator& operator++();
        const_iterator operator++(int);
        const_iterator& operator--();
        const_iterator operator--(int);
        const_iterator& operator+=(difference_type offset);
        const_iterator& operator-=(difference_type offset);
        friend const_iterator operator+(const_iterator value, difference_type offset) {
            return value += offset;
        }
        friend const_iterator operator+(difference_type offset, const_iterator value) {
            return value += offset;
        }
        friend const_iterator operator-(const_iterator value, difference_type offset) {
            return value -= offset;
        }
        friend difference_type operator-(const_iterator first, const_iterator second) {
            Q_ASSERT(first.m_owner == second.m_owner);
            return first.m_index - second.m_index;
        }
        friend bool operator==(const const_iterator&, const const_iterator&) = default;
        friend bool operator<(const_iterator first, const_iterator second) {
            Q_ASSERT(first.m_owner == second.m_owner);
            return first.m_index < second.m_index;
        }
        friend bool operator>(const_iterator first, const_iterator second) {
            return second < first;
        }
        friend bool operator<=(const_iterator first, const_iterator second) {
            return !(second < first);
        }
        friend bool operator>=(const_iterator first, const_iterator second) {
            return !(first < second);
        }

      private:
        friend class ImageSplitLines;
        const_iterator(const ImageSplitLines* owner, qsizetype index)
            : m_owner(owner), m_index(index) {}
        const ImageSplitLines* m_owner = nullptr;
        qsizetype m_index = 0;
    };

    class Reference final {
      public:
        operator int() const;
        Reference& operator=(int value);
        Reference& operator=(const Reference& other);

      private:
        friend class ImageSplitLines;
        Reference(ImageSplitLines* owner, qsizetype index) : m_owner(owner), m_index(index) {}
        ImageSplitLines* m_owner;
        qsizetype m_index;
    };

    ImageSplitLines() = default;
    ImageSplitLines(std::initializer_list<int> values);
    ImageSplitLines(const QVector<int>& values);
    [[nodiscard]] qsizetype size() const;
    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] int at(qsizetype index) const;
    [[nodiscard]] int operator[](qsizetype index) const;
    Reference operator[](qsizetype index);
    [[nodiscard]] int first() const;
    [[nodiscard]] int last() const;
    [[nodiscard]] bool contains(int coordinate) const;
    // Coordinate searches require sorted axes; fractional viewport coordinates are not rounded.
    [[nodiscard]] qsizetype coordinateLowerBound(double coordinate) const;
    [[nodiscard]] qsizetype coordinateUpperBound(double coordinate) const;
    [[nodiscard]] const_iterator begin() const;
    [[nodiscard]] const_iterator end() const;
    [[nodiscard]] const_iterator cbegin() const;
    [[nodiscard]] const_iterator cend() const;
    void reserve(qsizetype count);
    void append(int coordinate);
    void insert(qsizetype index, int coordinate);
    const_iterator insert(const_iterator position, int coordinate);
    void removeAt(qsizetype index);
    void clear();
    bool appendArithmetic(int start, int step, qsizetype count);
    [[nodiscard]] bool isValid(int dimension) const;
    [[nodiscard]] qsizetype runCount() const;
    [[nodiscard]] qsizetype storageBytes() const;

    friend bool operator==(const ImageSplitLines& first, const ImageSplitLines& second);
    friend bool operator==(const ImageSplitLines& first, const QVector<int>& second);
    friend bool operator==(const QVector<int>& first, const ImageSplitLines& second) {
        return second == first;
    }

  private:
    friend class ImageSplitDocument;
    struct Data;
    std::shared_ptr<const Data> m_data;
    void setAt(qsizetype index, int coordinate);
};

struct ImageSplitSettings final {
    ImageSplitDirection direction = ImageSplitDirection::Vertical;
    ImageSplitType type = ImageSplitType::Equal;
    ImageSplitUnit unit = ImageSplitUnit::Percentage;
    int count = 2;
    int rows = 2;
    int columns = 2;
    int pixels = 16;
    QString percentage = QStringLiteral("50");

    friend bool operator==(const ImageSplitSettings&, const ImageSplitSettings&) = default;
};

struct ImageSplitState final {
    ImageSplitSettings settings;
    ImageSplitLines verticalLines;
    ImageSplitLines horizontalLines;
    bool manuallyEdited = false;

    friend bool operator==(const ImageSplitState&, const ImageSplitState&) = default;
};

// Regions use half-open pixel boundaries. Rectangles are derived lazily so a dense grid does not
// allocate one rectangle for every output image.
struct ImageSplitLayout final {
    QSize size;
    ImageSplitLines verticalLines;
    ImageSplitLines horizontalLines;
    QMap<QString, QString> errors;

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] qint64 outputCount() const;
    [[nodiscard]] QRect regionAt(qint64 index) const;
};

// Immutable handoff: retain one source and compact guide axes until all pieces are imported.
struct ImageSplitSnapshot final {
    QImage image;
    ImageSplitLayout layout;
    QString imageName;
    std::shared_ptr<void> resourceOwner;
};

[[nodiscard]] ImageSplitLayout calculateImageSplitLayout(QSize size,
                                                         const ImageSplitSettings& settings);
[[nodiscard]] std::optional<int> imageSplitPercentagePixels(int dimension,
                                                            const QString& percentage);

class ImageSplitDocument final : public QObject {
    Q_OBJECT

  public:
    explicit ImageSplitDocument(QObject* parent = nullptr);
    ~ImageSplitDocument() override;
    [[nodiscard]] const QImage& image() const;
    [[nodiscard]] const QImage& preview() const;
    void setImage(QImage image, QImage preview = {}, ImageSplitSettings initialSettings = {});
    [[nodiscard]] ImageSplitSettings settings() const;
    [[nodiscard]] ImageSplitState state() const;
    [[nodiscard]] ImageSplitLayout layout() const;
    // History entry capacity and unique retained run nodes, including an active drag. Shared nodes
    // count once; image pixels, string allocations and shared_ptr control blocks are excluded.
    [[nodiscard]] qsizetype historyStorageBytes() const;
    bool setSettings(const ImageSplitSettings& settings);
    [[nodiscard]] bool canUndo() const;
    [[nodiscard]] bool canRedo() const;
    void undo();
    void redo();
    bool createLine(bool vertical, int coordinate);
    bool removeLine(bool vertical, int index);
    bool beginLineDrag(bool vertical, int index);
    bool moveDraggedLine(int coordinate);
    void endLineDrag();
    void cancelLineDrag();

  signals:
    void lineDragIndexChanged(int index);
    void lineMoved(bool vertical, int previousCoordinate, int coordinate);
    void changed();
    void historyChanged();

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

struct ImageSplitExportResult final {
    QString directory;
    QString error;
    qint64 savedCount = 0;
    qint64 totalCount = 0;
    QString errorSource{};
    QStringList errorArguments{};

    [[nodiscard]] bool succeeded() const;
    [[nodiscard]] QString errorMessage() const;
};

// Run on an export worker. Completed files remain available if a later region fails or the job is
// canceled. Only the immutable full-resolution source is shared; region rasters are streamed.
[[nodiscard]] ImageSplitExportResult
exportSplitImages(const QImage& image, const ImageSplitLayout& layout,
                  const QString& parentDirectory, const QString& imageName,
                  std::function<bool()> cancelled = {},
                  std::function<void(qint64, qint64)> progress = {});

} // namespace snow_shot::presentation
#endif
