#include "snow_shot/presentation/screenshottextannotation.h"
#include <QCoreApplication>
#include <cstdlib>
#include <iostream>

namespace {
using Tool = ScreenshotTextAnnotationTool;
using Style = ScreenshotTextAnnotationStyle;
using Range = ScreenshotTextSelectionRange;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void rangeMutations() {
    ScreenshotTextAnnotationDocument document;
    document.bindSource(ScreenshotTextAnnotationSource::Ocr, QByteArray("fixture"));
    const Style red{Qt::red}, blue{Qt::blue};
    require(document.toggle({{0, 0, 10}}, Tool::Highlight, red), "apply highlight");
    require(document.toggle({{0, 3, 7}}, Tool::Highlight, red), "remove middle");
    require(document.runs().size() == 2 && document.runs()[0].range.end == 3 &&
                document.runs()[1].range.start == 7,
            "removal splits original range");
    require(!document.hasStyle({{0, 0, 10}}, Tool::Highlight) &&
                document.hasStyle({{0, 0, 10}}, Tool::Highlight, false),
            "partial coverage is inactive but present");
    require(document.toggle({{0, 0, 10}}, Tool::Highlight, blue) && document.runs().size() == 1 &&
                document.runs()[0].highlight->color == Qt::blue,
            "partial click applies remembered color and merges");
    document.toggle({{0, 2, 8}}, Tool::Underline, red);
    document.toggle({{0, 4, 6}}, Tool::Strikethrough, blue);
    require(document.hasStyle({{0, 0, 10}}, Tool::Highlight) &&
                document.hasStyle({{0, 2, 4}}, Tool::Underline) &&
                document.hasStyle({{0, 4, 6}}, Tool::Strikethrough) &&
                !document.hasStyle({{0, 4, 6}}, Tool::Underline, false),
            "line styles replace one another while retaining highlight");
    document.toggle({{0, 2, 8}}, Tool::WavyUnderline, red);
    require(document.hasStyle({{0, 2, 8}}, Tool::WavyUnderline) &&
                !document.hasStyle({{0, 2, 8}}, Tool::Underline, false) &&
                !document.hasStyle({{0, 2, 8}}, Tool::Strikethrough, false),
            "wave replaces both line kinds");
    document.toggle({{0, 2, 8}}, Tool::WavyUnderline, red);
    require(!document.hasStyle({{0, 0, 10}}, Tool::WavyUnderline, false) &&
                document.hasStyle({{0, 0, 10}}, Tool::Highlight),
            "active line removal leaves highlight");
    const auto revision = document.revision();
    require(!document.toggle({}, Tool::Highlight, red) && document.revision() == revision,
            "empty selections do not mutate");
    require(!document.hasStyle({{1, 0, 10}}, Tool::Highlight, false),
            "another passage remains unstyled");
}

void persistenceAndRecolor() {
    ScreenshotTextAnnotationDocument document;
    document.bindSource(ScreenshotTextAnnotationSource::Formatted, QByteArray("source"));
    const Style red{QColor(255, 0, 0, 90)}, blue{Qt::blue};
    document.toggle({{0, 0, 3}, {0, 7, 10}}, Tool::Underline, red);
    const auto bytes = document.serialize();
    ScreenshotTextAnnotationDocument restored;
    require(restored.restore(bytes) && restored.runs() == document.runs() &&
                restored.fingerprint() == QByteArray("source"),
            "logical ranges and full styles roundtrip");
    restored.updateStyle({{0, 2, 8}}, Tool::Underline, blue);
    require(restored.selectedStyle({{0, 0, 2}}, Tool::Underline)->color == red.color &&
                restored.selectedStyle({{0, 2, 3}}, Tool::Underline)->color == blue.color &&
                !restored.hasStyle({{0, 3, 7}}, Tool::Underline, false) &&
                restored.selectedStyle({{0, 7, 8}}, Tool::Underline)->color == blue.color &&
                restored.selectedStyle({{0, 8, 10}}, Tool::Underline)->color == red.color,
            "recolor clips to selected existing ranges without filling holes");
    bool mixed = false;
    static_cast<void>(restored.selectedStyle({{0, 0, 10}}, Tool::Underline, &mixed));
    require(mixed && !restored.hasStyle({{0, 0, 10}}, Tool::Underline),
            "mixed styles and coverage reported independently");
    const auto before = restored.runs();
    for (qsizetype size = 1; size < bytes.size(); ++size) {
        require(!restored.restore(bytes.first(size)) && restored.runs() == before,
                "truncated payload rejected atomically");
    }
    require(!restored.restore(bytes + 'x'), "trailing data rejected");
    restored.bindSource(ScreenshotTextAnnotationSource::Formatted, QByteArray("source"));
    require(!restored.empty(), "same source preserves marks");
    restored.bindSource(ScreenshotTextAnnotationSource::Formatted, QByteArray("new source"));
    require(restored.empty() && restored.serialize().isEmpty(), "changed source clears payload");
    require(restored.restore({}) && restored.empty(), "legacy absent payload supported");
}

void normalizedRanges() {
    ScreenshotTextAnnotationDocument document;
    document.toggle({{1, 2, 5}, {0, 4, 8}, {0, 0, 5}, {0, -1, 2}, {2, 1, 1}}, Tool::Highlight,
                    Style{});
    require(document.runs().size() == 2 && document.runs()[0].range == Range{0, 0, 8} &&
                document.runs()[1].range == Range{1, 2, 5},
            "out-of-order and overlapping ranges normalize");
    require(document.hasStyle({{0, 0, 8}, {1, 2, 5}}, Tool::Highlight),
            "all selected source lines have coverage");
    require(!document.hasStyle({{0, 0, 8}, {1, 2, 6}}, Tool::Highlight),
            "coverage across source lines detects final gap");
    require(document.validForSource({8, 5}) && !document.validForSource({7, 5}) &&
                !document.validForSource({8}),
            "restored ranges must fit their bound source");
}

void colorEditsPreserveStrokeProperties() {
    ScreenshotTextAnnotationDocument document;
    Style dashed{Qt::red};
    dashed.strokeStyle = SnowCanvasStrokeStyle::Dashed;
    dashed.renderingStyle = SnowCanvasStrokeRenderingStyle::Artist;
    document.toggle({{0, 0, 3}}, Tool::Underline, dashed);
    document.toggle({{0, 4, 7}}, Tool::Underline, Style{Qt::green});
    document.updateStyle({{0, 0, 7}}, Tool::Underline, Style{Qt::blue},
                         ScreenshotTextAnnotationStylePropertyColor);
    const auto first = document.selectedStyle({{0, 0, 3}}, Tool::Underline);
    const auto second = document.selectedStyle({{0, 4, 7}}, Tool::Underline);
    quint32 mixedProperties = 0;
    static_cast<void>(
        document.selectedStyle({{0, 0, 7}}, Tool::Underline, nullptr, &mixedProperties));
    require(first->color == Qt::blue && second->color == Qt::blue &&
                first->strokeStyle == SnowCanvasStrokeStyle::Dashed &&
                first->renderingStyle == SnowCanvasStrokeRenderingStyle::Artist &&
                second->strokeStyle == SnowCanvasStrokeStyle::Solid,
            "color edits preserve each annotation's other properties");
    require(!(mixedProperties & ScreenshotTextAnnotationStylePropertyColor) &&
                (mixedProperties & ScreenshotTextAnnotationStylePropertyStrokeStyle) &&
                (mixedProperties & ScreenshotTextAnnotationStylePropertyRenderingStyle),
            "color becomes uniform while other style properties stay mixed");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    rangeMutations();
    persistenceAndRecolor();
    normalizedRanges();
    colorEditsPreserveStrokeProperties();
    std::cout << "Text annotation model tests passed\n";
    return 0;
}
