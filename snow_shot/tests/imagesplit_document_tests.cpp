// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/imagesplitdocument.h"

#include <QCoreApplication>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace snow_shot::presentation;
namespace {
void require(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}

QImage fixture(QSize size) {
    QImage image(size, QImage::Format_RGBA8888);
    image.fill(QColor(37, 91, 145, 123));
    return image;
}

void initialSettingsEstablishUndoBaseline() {
    ImageSplitDocument document;
    ImageSplitSettings initial;
    initial.direction = ImageSplitDirection::Horizontal;
    document.setImage(fixture({80, 120}), {}, initial);
    require(document.settings() == initial && !document.canUndo() && !document.canRedo() &&
                document.layout().regionAt(0) == QRect(0, 0, 80, 60),
            "initial settings establish the horizontal layout without a synthetic undo entry");
    auto changed = initial;
    changed.direction = ImageSplitDirection::Vertical;
    require(document.setSettings(changed), "initial direction remains editable");
    document.undo();
    require(document.settings() == initial && !document.canUndo() && document.canRedo(),
            "undo returns to the requested initial direction");
    document.setImage(fixture({80, 120}));
    require(document.settings() == ImageSplitSettings{} && !document.canUndo() &&
                !document.canRedo(),
            "ordinary image initialization retains default settings and resets history");
}

void requireCoverage(const ImageSplitLayout& layout) {
    require(layout.isValid(), "coverage fixture is valid");
    std::vector<int> visits(size_t(layout.size.width()) * size_t(layout.size.height()), 0);
    for (qint64 index = 0; index < layout.outputCount(); ++index) {
        const QRect region = layout.regionAt(index);
        require(region.width() > 0 && region.height() > 0, "all regions contain pixels");
        require(QRect(QPoint{}, layout.size).contains(region), "all regions lie inside image");
        for (int y = region.y(); y < region.y() + region.height(); ++y)
            for (int x = region.x(); x < region.x() + region.width(); ++x)
                ++visits[size_t(y) * size_t(layout.size.width()) + size_t(x)];
    }
    require(std::all_of(visits.begin(), visits.end(), [](int count) { return count == 1; }),
            "every source pixel belongs to exactly one region");
    require(layout.regionAt(-1).isEmpty() && layout.regionAt(layout.outputCount()).isEmpty(),
            "out-of-range regions are empty");
}

void equalAndGrid() {
    ImageSplitSettings settings;
    settings.count = 3;
    auto layout = calculateImageSplitLayout({1200, 900}, settings);
    require(layout.isValid() && layout.outputCount() == 3 &&
                layout.regionAt(0) == QRect(0, 0, 400, 900) &&
                layout.regionAt(2) == QRect(800, 0, 400, 900),
            "vertical lines partition left to right at source resolution");
    settings.direction = ImageSplitDirection::Horizontal;
    layout = calculateImageSplitLayout({1200, 901}, settings);
    require(layout.regionAt(0) == QRect(0, 0, 1200, 301) &&
                layout.regionAt(1) == QRect(0, 301, 1200, 300) &&
                layout.regionAt(2) == QRect(0, 601, 1200, 300),
            "equal split assigns leading extras and covers an odd height");
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 2;
    settings.columns = 3;
    settings.type = ImageSplitType::Precise;
    settings.pixels = 0;
    settings.percentage = QStringLiteral("invalid hidden input");
    layout = calculateImageSplitLayout({7, 5}, settings);
    require(layout.outputCount() == 6 && layout.regionAt(0) == QRect(0, 0, 3, 3) &&
                layout.regionAt(2) == QRect(5, 0, 2, 3) &&
                layout.regionAt(3) == QRect(0, 3, 3, 2) && layout.regionAt(5) == QRect(5, 3, 2, 2),
            "rectangular grids use independent dimension distribution and row-major order");
    requireCoverage(layout);
    for (int dimension = 1; dimension <= 35; ++dimension) {
        settings.direction = ImageSplitDirection::Vertical;
        settings.type = ImageSplitType::Equal;
        for (int count = 1; count <= dimension; ++count) {
            settings.count = count;
            layout = calculateImageSplitLayout({dimension, 3}, settings);
            requireCoverage(layout);
            int minimum = dimension;
            int maximum = 0;
            for (qint64 index = 0; index < layout.outputCount(); ++index) {
                minimum = std::min(minimum, layout.regionAt(index).width());
                maximum = std::max(maximum, layout.regionAt(index).width());
            }
            require(maximum - minimum <= 1, "equal section sizes differ by at most one pixel");
        }
    }
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 2000;
    settings.columns = 32000;
    layout = calculateImageSplitLayout({32000, 2000}, settings);
    require(layout.isValid() && layout.outputCount() == 64'000'000 &&
                layout.regionAt(63'999'999) == QRect(31999, 1999, 1, 1),
            "millions of regions use lazy rectangles with 64-bit indexing");
}

void compactAxes() {
    ImageSplitLines dense;
    require(dense.appendArithmetic(1, 1, 63'999'999) && dense.runCount() == 1 &&
                dense.storageBytes() < 1024 && dense.size() == 63'999'999 &&
                dense.isValid(64'000'000) && dense.first() == 1 && dense.last() == 63'999'999,
            "an extreme admitted axis stores millions of guides in one bounded arithmetic run");
    require(std::lower_bound(dense.cbegin(), dense.cend(), 32'000'000) - dense.cbegin() ==
                    31'999'999 &&
                std::upper_bound(dense.cbegin(), dense.cend(), 32'000'000) - dense.cbegin() ==
                    32'000'000 &&
                dense.contains(32'000'000) && !dense.contains(64'000'000),
            "random-access guide iterators support logarithmic coordinate lookup");
    const auto original = dense;
    dense.removeAt(31'999'999);
    require(dense.size() == 63'999'998 && dense.at(31'999'999) == 32'000'001 &&
                dense.isValid(64'000'000) && original.size() == 63'999'999,
            "manual deletion splits a run and preserves immutable snapshots");
    dense.insert(31'999'999, 32'000'000);
    require(dense == original, "delete and reinsert restores logical equality without full scans");
    ImageSplitLines segmented;
    require(segmented.appendArithmetic(2, 2, 2) && segmented.appendArithmetic(6, 3, 2),
            "piecewise guide fixture is representable");
    const ImageSplitLines appended{2, 4, 6, 9};
    require(segmented == appended && appended == QVector<int>{2, 4, 6, 9},
            "different run decompositions compare by logical coordinates");
    require(!segmented.appendArithmetic(std::numeric_limits<int>::max(), 1, 2),
            "overflowing arithmetic endpoints are rejected before mutation");
    ImageSplitSettings settings;
    settings.count = 64'000'000;
    const auto extreme = calculateImageSplitLayout({64'000'000, 1}, settings);
    require(extreme.isValid() && extreme.outputCount() == 64'000'000 &&
                extreme.verticalLines.storageBytes() < 1024 &&
                extreme.regionAt(63'999'999) == QRect(63'999'999, 0, 1, 1),
            "extreme aspect-ratio equal splitting is lazy and covers the final pixel");
    settings.type = ImageSplitType::Precise;
    settings.unit = ImageSplitUnit::Pixels;
    settings.pixels = 16;
    const auto precise = calculateImageSplitLayout({64'000'000, 1}, settings);
    require(precise.isValid() && precise.outputCount() == 4'000'000 &&
                precise.verticalLines.runCount() == 1 &&
                precise.verticalLines.storageBytes() < 1024,
            "millions of precise pixel guides also occupy one bounded run");
    settings.type = ImageSplitType::Equal;
    settings.count = 1;
    require(calculateImageSplitLayout({std::numeric_limits<int>::max(), 1}, settings).isValid(),
            "a single section at INT_MAX avoids arithmetic overflow");
    ImageSplitLines edited;
    QVector<int> oracle;
    quint32 random = 0x51337u;
    const auto next = [&random] {
        random = random * 1664525u + 1013904223u;
        return random;
    };
    for (int operation = 0; operation < 2000; ++operation) {
        const int choice = int(next() % 3);
        if (choice == 0 || oracle.isEmpty()) {
            const int coordinate = 1 + int(next() % 999);
            const auto position = std::lower_bound(oracle.cbegin(), oracle.cend(), coordinate);
            if (position == oracle.cend() || *position != coordinate) {
                const qsizetype index = position - oracle.cbegin();
                oracle.insert(index, coordinate);
                edited.insert(index, coordinate);
            }
        } else if (choice == 1) {
            const qsizetype index = qsizetype(next()) % oracle.size();
            oracle.removeAt(index);
            edited.removeAt(index);
        } else {
            const qsizetype index = qsizetype(next()) % oracle.size();
            const int minimum = index == 0 ? 1 : oracle.at(index - 1) + 1;
            const int maximum = index + 1 == oracle.size() ? 999 : oracle.at(index + 1) - 1;
            const int coordinate = minimum + int(next() % quint32(maximum - minimum + 1));
            oracle[index] = coordinate;
            edited[index] = coordinate;
        }
        require(edited == oracle && edited.isValid(1000),
                "run mutations match a deterministic sorted-vector oracle");
        const int coordinate = int(next() % 1001);
        require(std::lower_bound(edited.cbegin(), edited.cend(), coordinate) - edited.cbegin() ==
                    std::lower_bound(oracle.cbegin(), oracle.cend(), coordinate) - oracle.cbegin(),
                "edited run lookup agrees with vector lookup after every mutation");
        for (const double threshold :
             {double(coordinate), double(coordinate) - 0.25, double(coordinate) + 0.25}) {
            require(edited.coordinateLowerBound(threshold) ==
                            std::lower_bound(oracle.cbegin(), oracle.cend(), threshold) -
                                oracle.cbegin() &&
                        edited.coordinateUpperBound(threshold) ==
                            std::upper_bound(oracle.cbegin(), oracle.cend(), threshold) -
                                oracle.cbegin(),
                    "run-aware fractional coordinate searches match sorted-vector bounds");
        }
    }
}

void persistentRunSequences() {
    ImageSplitLines edited;
    QVector<int> oracle;
    std::vector<std::pair<ImageSplitLines, QVector<int>>> snapshots;
    quint32 random = 0x41823u;
    const auto next = [&random] {
        random = random * 1664525u + 1013904223u;
        return random;
    };
    const auto coordinate = [&next] {
        switch (next() % 8) {
        case 0:
            return std::numeric_limits<int>::min();
        case 1:
            return std::numeric_limits<int>::max();
        default:
            return int(next() % 2001) - 1000;
        }
    };
    for (int operation = 0; operation < 10'000; ++operation) {
        const int choice = int(next() % 5);
        if (choice == 0 || oracle.isEmpty()) {
            const qsizetype index = qsizetype(next()) % (oracle.size() + 1);
            const int value = coordinate();
            edited.insert(index, value);
            oracle.insert(index, value);
        } else if (choice == 1) {
            const qsizetype index = qsizetype(next()) % oracle.size();
            edited.removeAt(index);
            oracle.removeAt(index);
        } else if (choice == 2) {
            const qsizetype index = qsizetype(next()) % oracle.size();
            const int value = coordinate();
            edited[index] = value;
            oracle[index] = value;
        } else if (choice == 3) {
            const int start = int(next() % 2001) - 1000;
            const int step = int(next() % 9) - 4;
            const qsizetype count = next() % 7;
            require(edited.appendArithmetic(start, step, count),
                    "small ascending, descending and constant runs append");
            for (qsizetype index = 0; index < count; ++index)
                oracle.append(start + int(index) * step);
        } else if (oracle.size() > 100) {
            edited.clear();
            oracle.clear();
        }
        const ImageSplitLines rebuilt(oracle);
        require(edited == oracle && edited == rebuilt && rebuilt == edited,
                "persistent sequence operations preserve arbitrary signed vector coordinates");
        const bool valid = oracle.size() < 1000 &&
                           std::adjacent_find(
                               oracle.cbegin(), oracle.cend(),
                               [](int a, int b) { return a >= b; }) == oracle.cend() &&
                           (oracle.isEmpty() || (oracle.first() > 0 && oracle.last() < 1000));
        require(edited.isValid(1000) == valid,
                "cached validity rejects unsorted, duplicate and out-of-range axes");
        const int query = coordinate();
        require(edited.contains(query) ==
                    (std::find(oracle.cbegin(), oracle.cend(), query) != oracle.cend()),
                "coordinate membership remains correct for arbitrary signed run sequences");
        if (operation % 97 == 0)
            snapshots.emplace_back(edited, oracle);
    }
    for (const auto& [snapshot, expected] : snapshots)
        require(snapshot == expected && snapshot == ImageSplitLines(expected),
                "retained sequence snapshots survive all later mutations");

    require(ImageSplitLines{10, 20, 30} != ImageSplitLines{11, 18, 31},
            "equal sum and weighted signatures still require exact coordinate comparison");
    ImageSplitLines fullRange;
    const qsizetype fullCount = qsizetype(std::numeric_limits<quint32>::max()) + 1;
    require(fullRange.appendArithmetic(std::numeric_limits<int>::min(), 1, fullCount) &&
                fullRange.first() == std::numeric_limits<int>::min() &&
                fullRange.last() == std::numeric_limits<int>::max() &&
                fullRange.coordinateLowerBound(0) == fullCount / 2 &&
                fullRange.coordinateUpperBound(0) == fullCount / 2 + 1,
            "rank searches preserve the full signed integer coordinate range without overflow");
    for (const double threshold :
         {-std::numeric_limits<double>::infinity(), -100.25, 0.0, std::nextafter(0.0, 1.0), 100.75,
          std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        const QVector<int> sorted{-100, -1, 0, 1, 100};
        const ImageSplitLines lines(sorted);
        require(
            lines.coordinateLowerBound(threshold) ==
                    std::lower_bound(sorted.cbegin(), sorted.cend(), threshold) - sorted.cbegin() &&
                lines.coordinateUpperBound(threshold) ==
                    std::upper_bound(sorted.cbegin(), sorted.cend(), threshold) - sorted.cbegin(),
            "coordinate searches preserve fractional and nonfinite comparison semantics");
    }
    ImageSplitLines maximum;
    require(maximum.appendArithmetic(1, 0, std::numeric_limits<qsizetype>::max()),
            "constant run counts fit the complete sequence index range");
    const auto unchanged = maximum;
    require(!maximum.appendArithmetic(2, 0, 1) && maximum == unchanged && maximum.last() == 1 &&
                !maximum.isValid(std::numeric_limits<int>::max()),
            "count overflow is rejected without corrupting retained sequence signatures");
}

void fragmentedHistory() {
    constexpr int width = 80'000;
    constexpr int edits = 4096;
    ImageSplitDocument document;
    document.setImage(fixture({width, 1}));
    auto settings = document.settings();
    settings.count = width;
    require(document.setSettings(settings), "fragmented history starts from a compact dense axis");
    const auto original = document.state();
    qsizetype halfBytes = 0;
    for (int index = 0; index < edits; ++index) {
        const int coordinate = 16 * (index + 1);
        const auto lines = document.layout().verticalLines;
        const auto position = lines.coordinateLowerBound(coordinate);
        require(position < lines.size() && lines.at(position) == coordinate &&
                    document.removeLine(true, int(position)),
                "each spaced deletion creates an independent history operation");
        if (index + 1 == edits / 2)
            halfBytes = document.historyStorageBytes();
    }
    const auto final = document.state();
    require(final.verticalLines.runCount() >= edits &&
                document.historyStorageBytes() < 16 * 1024 * 1024 &&
                document.historyStorageBytes() < 3 * halfBytes,
            "fragmented undo history shares tree paths instead of copying every previous run");
    for (int index = edits - 1; index >= 0; --index) {
        document.undo();
        const auto lines = document.layout().verticalLines;
        require(lines.size() == width - 1 - index && lines.contains(16 * (index + 1)) &&
                    (index == 0 || !lines.contains(16 * index)),
                "every spaced deletion remains individually undoable in reverse order");
    }
    require(document.state() == original, "all undos restore the original compact generated axis");
    for (int index = 0; index < edits; ++index) {
        document.redo();
        const auto lines = document.layout().verticalLines;
        require(lines.size() == width - 2 - index && !lines.contains(16 * (index + 1)),
                "all fragmented edit history entries remain individually redoable");
    }
    require(document.state() == final && !document.canRedo(),
            "complete redo restores the exact retained fragmented state");
    document.undo();
    require(document.canRedo() && !document.layout().verticalLines.contains(16),
            "a history branch restores a genuinely missing guide while redo is available");
    const auto beforeBranch = document.state();
    require(document.createLine(true, 16) && !document.canRedo() &&
                document.layout().verticalLines.contains(16) && document.layout().isValid(),
            "a branch after undo discards only redo entries and keeps the edited tree valid");
    document.undo();
    require(document.state() == beforeBranch && document.canRedo(),
            "branch insertion retains the complete preceding history state");
    document.redo();
    require(document.layout().verticalLines.contains(16) && !document.canRedo(),
            "redo follows the new branch after old redo entries are discarded");
    document.setImage(fixture({20, 1}));
    require(document.historyStorageBytes() < 4096 && !document.canUndo(),
            "replacement releases all previous persistent history roots");
}

void exactPercentageConversion() {
    require(imageSplitPercentagePixels(1200, QStringLiteral("5.125")) == 62 &&
                imageSplitPercentagePixels(1200, QStringLiteral("5.124999999999999999999")) == 61 &&
                imageSplitPercentagePixels(500, QStringLiteral("5.3")) == 27,
            "unit conversion uses exact arbitrary decimal half-up rounding");
    require(imageSplitPercentagePixels(100, QStringLiteral("300")) == 300 &&
                imageSplitPercentagePixels(100, QStringLiteral("0")) == 0,
            "permissive conversion preserves nonnegative out-of-range percentage drafts");
    require(!imageSplitPercentagePixels(100, QStringLiteral("-5")) &&
                !imageSplitPercentagePixels(0, QStringLiteral("50")) &&
                !imageSplitPercentagePixels(100, QStringLiteral("2147483648")),
            "invalid or overflowing unit conversions fail without clamping");
    ImageSplitSettings settings;
    settings.type = ImageSplitType::Precise;
    settings.percentage = QStringLiteral("5.125");
    require(calculateImageSplitLayout({1200, 800}, settings).verticalLines.first() == 62,
            "precise settings accept fractions beyond two decimal places");
}

void preciseAndValidation() {
    ImageSplitSettings settings;
    settings.type = ImageSplitType::Precise;
    settings.unit = ImageSplitUnit::Pixels;
    settings.pixels = 300;
    auto layout = calculateImageSplitLayout({1000, 7}, settings);
    require(layout.verticalLines == QVector<int>{300, 600, 900} &&
                layout.regionAt(3).width() == 100,
            "precise pixel splitting preserves a smaller final section");
    settings.direction = ImageSplitDirection::Horizontal;
    layout = calculateImageSplitLayout({7, 1000}, settings);
    require(layout.horizontalLines == QVector<int>{300, 600, 900} &&
                layout.regionAt(3).height() == 100,
            "precise horizontal splitting starts at the top");
    settings.direction = ImageSplitDirection::Vertical;
    settings.pixels = 1000;
    require(calculateImageSplitLayout({1000, 7}, settings).outputCount() == 1,
            "a full-dimension interval produces one image");
    settings.pixels = 2000;
    require(calculateImageSplitLayout({1000, 7}, settings).outputCount() == 1,
            "an interval larger than the image preserves the single remainder");
    settings.pixels = 15;
    require(
        calculateImageSplitLayout({1000, 7}, settings).errors.contains(QStringLiteral("pixels")),
        "the precise pixel minimum has a field-specific error");
    settings.unit = ImageSplitUnit::Percentage;
    settings.percentage = QStringLiteral("25");
    layout = calculateImageSplitLayout({1001, 7}, settings);
    require(layout.verticalLines == QVector<int>{250, 501, 751} &&
                layout.regionAt(0).width() == 250 && layout.regionAt(1).width() == 251 &&
                layout.regionAt(2).width() == 250 && layout.regionAt(3).width() == 250,
            "percentage boundaries round individually with positive halves upward");
    settings.percentage = QStringLiteral("5.3");
    layout = calculateImageSplitLayout({500, 1}, settings);
    require(layout.verticalLines.first() == 27 && layout.verticalLines.at(2) == 80,
            "exact decimals round half pixels without binary floating-point drift");
    settings.percentage = QStringLiteral("5.000000000000000000000000000000001");
    require(calculateImageSplitLayout({20, 1}, settings).outputCount() == 20,
            "long exact decimal percentages do not require floating-point conversion");
    settings.percentage = QStringLiteral("100");
    require(calculateImageSplitLayout({1, 1}, settings).outputCount() == 1,
            "100 percent permits one-pixel input and one output");
    settings.percentage = QStringLiteral("99.99");
    const auto collapsedFinal = calculateImageSplitLayout({100, 1}, settings);
    require(!collapsedFinal.isValid() && collapsedFinal.outputCount() == 0 &&
                collapsedFinal.errors.contains(QStringLiteral("percentage")),
            "a required interior boundary rounded to the final edge rejects the empty remainder");
    settings.percentage = QStringLiteral("33.33");
    require(!calculateImageSplitLayout({1001, 1}, settings).isValid(),
            "successive percentage multiples reject a collapsed fractional final section");
    settings.percentage = QStringLiteral("99");
    const auto onePixelRemainder = calculateImageSplitLayout({100, 1}, settings);
    require(onePixelRemainder.isValid() && onePixelRemainder.outputCount() == 2 &&
                onePixelRemainder.regionAt(1).width() == 1,
            "a genuine one-pixel remainder remains a separate valid output");
    for (const char* invalid : {"0", "-5", "4.99", "100.01", "101", "NaN", "5x", "5.2.3"}) {
        settings.percentage = QString::fromLatin1(invalid);
        const auto invalidLayout = calculateImageSplitLayout({100, 1}, settings);
        require(!invalidLayout.isValid() && invalidLayout.outputCount() == 0 &&
                    invalidLayout.errors.contains(QStringLiteral("percentage")),
                "invalid percentages disable export and identify their input");
    }
    settings.percentage = QStringLiteral("5");
    require(!calculateImageSplitLayout({19, 1}, settings).isValid(),
            "subpixel percentage intervals are rejected");
    settings.type = ImageSplitType::Equal;
    for (const int invalid : {-1, 0, 101}) {
        settings.count = invalid;
        require(
            calculateImageSplitLayout({100, 1}, settings).errors.contains(QStringLiteral("count")),
            "equal counts must be positive and no greater than dimension");
    }
    settings.direction = ImageSplitDirection::Grid;
    settings.rows = 6;
    settings.columns = 8;
    const auto invalidGrid = calculateImageSplitLayout({7, 5}, settings);
    require(invalidGrid.errors.contains(QStringLiteral("rows")) &&
                invalidGrid.errors.contains(QStringLiteral("columns")),
            "both invalid grid inputs report errors independently");
    require(!calculateImageSplitLayout({}, {}).isValid(), "missing image has no valid layout");
    settings.direction = ImageSplitDirection::Vertical;
    settings.type = ImageSplitType::Precise;
    settings.unit = ImageSplitUnit::Percentage;
    for (int dimension = 20; dimension < 60; ++dimension)
        for (int percentage = 5; percentage <= 100; percentage += 5) {
            settings.percentage = QString::number(percentage);
            requireCoverage(calculateImageSplitLayout({dimension, 2}, settings));
        }
}

void historyAndManualEditing() {
    ImageSplitDocument document;
    require(!document.layout().isValid() && !document.canUndo() && !document.canRedo(),
            "empty document has no history or output");
    QImage source = fixture({100, 60});
    source.setDevicePixelRatio(2);
    const QByteArray original(reinterpret_cast<const char*>(source.constBits()),
                              source.sizeInBytes());
    document.setImage(source, fixture({20, 12}));
    require(document.settings() == ImageSplitSettings{} && document.layout().outputCount() == 2 &&
                document.layout().regionAt(0) == QRect(0, 0, 50, 60) &&
                document.preview().size() == QSize(20, 12) && !document.canUndo(),
            "opening resets defaults and uses physical source pixels independent of DPR");
    require(!document.createLine(false, 30) && !document.createLine(true, 0) &&
                !document.createLine(true, 100) && !document.createLine(true, 50) &&
                !document.canUndo(),
            "restricted, duplicate, and edge lines do not create operations");
    require(document.createLine(true, 20) && document.layout().outputCount() == 3 &&
                document.state().manuallyEdited,
            "manual insertion updates actual output count without enforcing equal count");
    const auto inserted = document.state();
    require(document.beginLineDrag(true, 1) && !document.canUndo(),
            "line movement starts an atomic gesture");
    document.moveDraggedLine(21);
    document.moveDraggedLine(1);
    require(document.layout().verticalLines == QVector<int>{1, 20},
            "dragging crosses neighboring lines and keeps cuts sorted");
    document.moveDraggedLine(1000);
    require(document.layout().verticalLines.last() == 99, "dragging clamps to image interior");
    document.endLineDrag();
    const auto dragged = document.state();
    document.undo();
    require(document.state() == inserted, "one undo reverts the complete drag gesture");
    document.redo();
    require(document.state() == dragged, "redo restores the final drag coordinate");
    require(document.beginLineDrag(true, 1), "existing line can be dragged again");
    document.moveDraggedLine(30);
    document.cancelLineDrag();
    require(document.state() == dragged, "canceling a gesture restores its original state");
    require(document.beginLineDrag(true, 1), "no-op gesture starts");
    document.moveDraggedLine(40);
    document.moveDraggedLine(99);
    document.endLineDrag();
    document.undo();
    require(document.state() == inserted, "dragging away and back does not create history");
    document.redo();
    auto settings = document.settings();
    settings.count = 4;
    require(document.setSettings(settings) &&
                document.layout().verticalLines == QVector<int>{25, 50, 75} &&
                !document.state().manuallyEdited,
            "parameter change replaces all existing manual lines");
    document.undo();
    require(document.state() == dragged, "undo regeneration restores settings and manual lines");
    require(document.removeLine(true, 0) && !document.canRedo(),
            "a new manual operation clears redo history");
    require(document.layout().outputCount() == 2 && document.layout().verticalLines.first() == 99,
            "manual deletion updates export geometry and count");
    const auto beforeInvalid = document.state();
    settings.count = 101;
    require(!document.setSettings(settings) && document.state() == beforeInvalid,
            "invalid draft settings preserve the committed document");
    settings = document.settings();
    settings.direction = ImageSplitDirection::Grid;
    require(document.setSettings(settings) && document.createLine(true, 10) &&
                document.createLine(false, 10) && document.layout().outputCount() == 9,
            "grid editing permits both orientations and derives the actual product");
    requireCoverage(document.layout());
    require(QByteArray(reinterpret_cast<const char*>(document.image().constBits()),
                       document.image().sizeInBytes()) == original,
            "all splitting operations leave source pixels unchanged");
    document.setImage(fixture({1, 1}));
    require(document.settings() == ImageSplitSettings{} && !document.layout().isValid() &&
                !document.canUndo() && !document.canRedo() && !document.image().isNull(),
            "replacement resets history and retains tiny images with invalid default split");
    settings = document.settings();
    settings.count = 1;
    require(document.setSettings(settings) && document.layout().outputCount() == 1,
            "a tiny image becomes exportable with a valid single section");
    document.undo();
    require(document.settings().count == 2 && !document.layout().isValid() &&
                document.layout().outputCount() == 0 &&
                document.layout().errors.contains(QStringLiteral("count")),
            "undo restores the tiny image's invalid default parameters and validation errors");
    document.redo();
    require(document.settings().count == 1 && document.layout().isValid() &&
                document.layout().errors.isEmpty() && document.layout().outputCount() == 1,
            "redo restores a valid tiny-image split without stale errors");
}
void lineDragsCrossOtherLines() {
    for (const auto direction : {ImageSplitDirection::Vertical, ImageSplitDirection::Horizontal,
                                 ImageSplitDirection::Grid}) {
        for (const bool vertical : {true, false}) {
            if ((direction == ImageSplitDirection::Vertical && !vertical) ||
                (direction == ImageSplitDirection::Horizontal && vertical))
                continue;
            ImageSplitDocument document;
            document.setImage(fixture({100, 100}));
            auto settings = document.settings();
            settings.direction = direction;
            settings.count = 5;
            settings.rows = 5;
            settings.columns = 5;
            require(document.setSettings(settings), "crossing fixture settings apply");
            const auto original = document.state();
            const auto lines = [&] {
                const auto layout = document.layout();
                return vertical ? layout.verticalLines : layout.horizontalLines;
            };
            QObject::connect(&document, &ImageSplitDocument::changed, &document,
                             [&] { requireCoverage(document.layout()); });
            require(document.beginLineDrag(vertical, 1), "a middle line starts dragging");
            require(document.moveDraggedLine(95) && lines() == QVector<int>{20, 60, 80, 95},
                    "one movement can cross multiple lines toward the end of either axis");
            require(!document.moveDraggedLine(60) && lines() == QVector<int>{20, 60, 80, 95},
                    "occupied pixel boundaries do not create duplicate cuts or lose the drag");
            require(document.moveDraggedLine(10) && lines() == QVector<int>{10, 20, 60, 80},
                    "the same dragged line crosses back over multiple neighbors");
            require(document.moveDraggedLine(50) && lines() == QVector<int>{20, 50, 60, 80},
                    "continued movement follows the dragged line after its sorted index changes");
            require(document.moveDraggedLine(-100) && lines() == QVector<int>{1, 20, 60, 80} &&
                        document.moveDraggedLine(1000) && lines() == QVector<int>{20, 60, 80, 99},
                    "crossing drags clamp only at the image interior");
            document.endLineDrag();
            const auto moved = document.state();
            document.undo();
            require(document.state() == original, "one undo restores the entire crossing gesture");
            document.redo();
            require(document.state() == moved, "redo restores the crossed line geometry");
            require(document.beginLineDrag(vertical, 3) && document.moveDraggedLine(10),
                    "a committed line can cross back during another gesture");
            document.cancelLineDrag();
            require(document.state() == moved, "cancel restores the state before crossing");
            require(document.beginLineDrag(vertical, 3) && document.moveDraggedLine(10) &&
                        document.moveDraggedLine(99),
                    "a crossed line can return to its original coordinate");
            document.endLineDrag();
            document.undo();
            require(document.state() == original,
                    "crossing away and back does not add a redundant history entry");
        }
    }

    ImageSplitDocument dense;
    dense.setImage(fixture({20'000, 2}));
    auto settings = dense.settings();
    settings.count = 10'000;
    require(dense.setSettings(settings), "dense crossing fixture settings apply");
    const auto original = dense.state();
    require(dense.beginLineDrag(true, 0) && dense.moveDraggedLine(19'999) &&
                dense.layout().verticalLines.first() == 4 &&
                dense.layout().verticalLines.last() == 19'999 &&
                dense.layout().verticalLines.size() == 9'999 &&
                dense.layout().verticalLines.storageBytes() < 1024,
            "crossing thousands of generated guides preserves compact sorted axes");
    requireCoverage(dense.layout());
    dense.cancelLineDrag();
    require(dense.state() == original, "cancel restores the shared dense-axis snapshot");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        initialSettingsEstablishUndoBaseline();
        equalAndGrid();
        compactAxes();
        persistentRunSequences();
        fragmentedHistory();
        exactPercentageConversion();
        preciseAndValidation();
        historyAndManualEditing();
        lineDragsCrossOtherLines();
        std::cout << "Image split document tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
