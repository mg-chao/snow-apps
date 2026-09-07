#include "snow_shot/presentation/screenshotocrlayout.h"

#include <QCoreApplication>
#include <QStringList>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
ScreenshotOcrLine box(const QString& text, qreal x, qreal y, qreal width, qreal height = 20) {
    return {text,
            0.95,
            {QPointF(x, y), QPointF(x + width, y), QPointF(x + width, y + height),
             QPointF(x, y + height)}};
}
void expect(const QVector<ScreenshotOcrLine>& input, const QStringList& expected,
            const char* name) {
    const auto result = snow_shot::presentation::mergeOcrLayout(input);
    QStringList actual;
    for (const auto& line : result)
        actual.push_back(line.text);
    if (actual != expected) {
        std::cerr << name << ": " << actual.join(QStringLiteral(" | ")).toStdString() << '\n';
        std::exit(EXIT_FAILURE);
    }
}
} // namespace
int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    using snow_shot::presentation::mergeOcrLayout;
    // Fixtures adapted from STranslate OcrLayoutAnalyzerTests (MIT).
    const QVector<ScreenshotOcrLine> paragraph{
        box(QStringLiteral("This is the first line"), 0, 0, 180),
        box(QStringLiteral("continued on the next line"), 0, 24, 210)};
    expect(paragraph, {QStringLiteral("This is the first line continued on the next line")},
           "paragraph");
    const auto merged = mergeOcrLayout(paragraph);
    auto shifted = paragraph;
    const QPointF origin(-500, -300);
    for (auto& line : shifted)
        line.quad.translate(origin);
    const auto shiftedMerged = mergeOcrLayout(shifted, origin);
    require(shiftedMerged.size() == 1 && shiftedMerged[0].text == merged[0].text &&
                shiftedMerged[0].quad == merged[0].quad.translated(origin),
            "canvas origin must not change image layout decisions");
    require(merged.size() == 1 && merged[0].paragraph &&
                merged[0].quad.boundingRect() == QRectF(0, 0, 210, 44) &&
                merged[0].sourceLineQuads ==
                    QVector<QPolygonF>{paragraph[0].quad, paragraph[1].quad} &&
                paragraph[0].text == QStringLiteral("This is the first line") &&
                paragraph[0].sourceLineQuads.isEmpty(),
            "merge must retain source geometry without modifying original OCR");
    expect({box(QStringLiteral("Left first paragraph"), 0, 0, 170),
            box(QStringLiteral("continues here"), 0, 24, 135),
            box(QStringLiteral("Right column starts"), 300, 12, 170),
            box(QStringLiteral("continues separately"), 300, 36, 180),
            box(QStringLiteral("Left second paragraph"), 0, 70, 185),
            box(QStringLiteral("continues too"), 0, 94, 120)},
           {QStringLiteral("Left first paragraph continues here"),
            QStringLiteral("Left second paragraph continues too"),
            QStringLiteral("Right column starts continues separately")},
           "column reading order");
    expect({box(QStringLiteral("File"), 0, 0, 32), box(QStringLiteral("Edit"), 66, 0, 32),
            box(QStringLiteral("View"), 132, 0, 36)},
           {QStringLiteral("File"), QStringLiteral("Edit"), QStringLiteral("View")}, "menu items");
    expect({box(QStringLiteral("First name"), 0, 0, 100),
            box(QStringLiteral("Order status"), 150, 0, 110),
            box(QStringLiteral("Alice Smith"), 0, 28, 105),
            box(QStringLiteral("Active now"), 150, 28, 95),
            box(QStringLiteral("Bob Stone"), 0, 56, 90),
            box(QStringLiteral("Paused now"), 150, 56, 96)},
           {QStringLiteral("First name"), QStringLiteral("Alice Smith"),
            QStringLiteral("Bob Stone"), QStringLiteral("Order status"),
            QStringLiteral("Active now"), QStringLiteral("Paused now")},
           "table cells");
    expect({box(QStringLiteral("This is trans-"), 0, 0, 160),
            box(QStringLiteral("lation continued"), 0, 24, 180)},
           {QStringLiteral("This is translation continued")}, "hyphenated continuation");
    expect({box(QStringLiteral("Hello"), 0, 0, 50), box(QStringLiteral("world"), 56, 0, 50)},
           {QStringLiteral("Hello world")}, "Latin word spacing");
    expect({box(QStringLiteral("\u4f60\u597d"), 0, 0, 40),
            box(QStringLiteral("\u4e16\u754c"), 44, 0, 40)},
           {QStringLiteral("\u4f60\u597d\u4e16\u754c")}, "CJK word spacing");
    expect(
        {box(QStringLiteral("1. First item"), 0, 0, 150),
         box(QStringLiteral("continued description"), 20, 24, 200),
         box(QStringLiteral("2. Second item"), 0, 48, 150)},
        {QStringLiteral("1. First item continued description"), QStringLiteral("2. Second item")},
        "list continuation");
    expect({box(QStringLiteral("1. First\ncontinuation\n2. Second\n3. Third"), 0, 0, 200, 80)},
           {QStringLiteral("1. First continuation"), QStringLiteral("2. Second"),
            QStringLiteral("3. Third")},
           "embedded list");
    const QString ordinary = QStringLiteral("Ordinary multiline\nOCR block");
    expect({box(ordinary, 0, 0, 200, 40)}, {ordinary}, "ordinary multiline block");
    const QString numbers = QStringLiteral("1. First\n3. Third");
    expect({box(numbers, 0, 0, 200, 40)}, {numbers}, "nonsequential list");
    expect({box(QStringLiteral("A large heading"), 0, 0, 240, 36),
            box(QStringLiteral("Body starts below"), 0, 40, 200)},
           {QStringLiteral("A large heading"), QStringLiteral("Body starts below")}, "heading");
    expect({box(QStringLiteral("The first paragraph ends."), 0, 0, 240),
            box(QStringLiteral("Another paragraph starts here"), 0, 38, 250)},
           {QStringLiteral("The first paragraph ends."),
            QStringLiteral("Another paragraph starts here")},
           "paragraph gap");
    auto vertical = box(QStringLiteral("vertical text"), 300, 0, 20, 160);
    vertical.direction = ScreenshotOcrTextDirection::Vertical;
    auto rotated = box(QStringLiteral("rotated text"), 400, 0, 100);
    rotated.quad = QTransform().rotate(20).map(rotated.quad);
    ScreenshotOcrLine missing;
    missing.text = QStringLiteral("no coordinates");
    auto invalid = box(QStringLiteral("invalid coordinates"), 0, 0, 10);
    invalid.quad[0].setX(std::numeric_limits<qreal>::quiet_NaN());
    auto mixed = paragraph;
    mixed += {vertical, rotated, missing, invalid};
    const auto preserved = mergeOcrLayout(mixed);
    require(preserved.size() == 5 && preserved[1].quad == vertical.quad &&
                preserved[2].quad == rotated.quad && preserved[3].text == missing.text &&
                preserved[4].text == invalid.text,
            "unsupported geometry must never lose OCR text");
    require(mergeOcrLayout({}).isEmpty(), "empty input");
    QVector<ScreenshotOcrLine> touchingTable;
    expect({box(QStringLiteral("Column one starts here"), 0, 0, 205),
            box(QStringLiteral("Column two starts here"), 300, 0, 205),
            box(QStringLiteral("Column three starts here"), 600, 0, 225),
            box(QStringLiteral("and carries the thought"), 0, 24, 210),
            box(QStringLiteral("with the next sentence"), 300, 24, 210),
            box(QStringLiteral("through another line"), 600, 24, 185),
            box(QStringLiteral("before ending normally"), 0, 48, 210),
            box(QStringLiteral("inside the same column"), 300, 48, 215),
            box(QStringLiteral("without table spacing"), 600, 48, 195)},
           {QStringLiteral("Column one starts here and carries the thought before ending normally"),
            QStringLiteral("Column two starts here with the next sentence inside the same column"),
            QStringLiteral("Column three starts here through another line without table spacing")},
           "multi-column body must not become a table");
    expect({box(QStringLiteral("first paragraph starts with an indent"), 24, 0, 420, 30),
            box(QStringLiteral("continues on the full body width"), 0, 36, 500, 30),
            box(QStringLiteral("ends on a short last line"), 0, 72, 240, 30),
            box(QStringLiteral("next paragraph starts indented"), 24, 108, 430, 30),
            box(QStringLiteral("continues with body text"), 0, 144, 490, 30)},
           {QStringLiteral("first paragraph starts with an indent continues on the full body width "
                           "ends on a short last line"),
            QStringLiteral("next paragraph starts indented continues with body text")},
           "first-line indent");
    expect({box(QStringLiteral("*"), 0, 0, 20, 24),
            box(QStringLiteral("Advanced Paste"), 44, 0, 154, 24),
            box(QStringLiteral("*"), 300, 0, 20, 24),
            box(QStringLiteral("Always on Top"), 344, 0, 148, 24),
            box(QStringLiteral("*"), 0, 40, 20, 24),
            box(QStringLiteral("Color Picker"), 44, 40, 128, 24),
            box(QStringLiteral("*"), 300, 40, 20, 24),
            box(QStringLiteral("Command Palette"), 344, 40, 184, 24),
            box(QStringLiteral("*"), 0, 80, 20, 24),
            box(QStringLiteral("File Explorer Add-ons"), 44, 80, 212, 24),
            box(QStringLiteral("*"), 300, 80, 20, 24),
            box(QStringLiteral("File Locksmith"), 344, 80, 144, 24)},
           {QStringLiteral("*Advanced Paste"), QStringLiteral("*Color Picker"),
            QStringLiteral("*File Explorer Add-ons"), QStringLiteral("*Always on Top"),
            QStringLiteral("*Command Palette"), QStringLiteral("*File Locksmith")},
           "table leading adornments stay with text");
    for (const int y : {0, 20, 40, 68}) {
        for (const int x : {0, 250, 500}) {
            touchingTable.push_back(box(QStringLiteral("Several words in this cell"), x, y, 200));
        }
    }
    require(mergeOcrLayout(touchingTable).size() == 12,
            "STranslate medians ignore zero gaps when recognizing touching table rows");
    return EXIT_SUCCESS;
}
