#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARLAYOUTMODEL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARLAYOUTMODEL_H

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/storage/settingsadapters.h"
#include "antd_icons.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace snow_shot::presentation::toolbar_layout {

enum class Item {
    Shape,
    Arrow,
    Line,
    FreeDraw,
    Highlighter,
    Spotlight,
    Text,
    SerialNumber,
    Filter,
    Eraser,
    Watermark,
};

enum class Icon {
    Shape,
    Arrow,
    Line,
    FreeDraw,
    Highlight,
    Spotlight,
    Text,
    SerialNumber,
    Filter,
    Eraser,
    Watermark,
    BarcodeRecognition,
    TableRecognition,
    RecordScreen,
    PinToScreen,
    TextRecognition,
    TextTranslation,
    ScrollingScreenshot,
    SaveAsFile,
    Markdown,
    Html,
};

struct Descriptor {
    Item item = Item::Shape;
    const char* id = nullptr;
    const char* label = nullptr;
    Icon icon = Icon::Shape;
};

struct EditorDescriptor {
    const char* id = nullptr;
    const char* translationContext = nullptr;
    const char* label = nullptr;
    Icon icon = Icon::Shape;
};

[[nodiscard]] inline const QVector<Descriptor>& descriptors() {
    static const QVector<Descriptor> value{
        {Item::Shape, "shape", QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Shape"),
         Icon::Shape},
        {Item::Arrow, "arrow", QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Arrow"),
         Icon::Arrow},
        {Item::Line, "line", QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Line"),
         Icon::Line},
        {Item::FreeDraw, "free-draw",
         QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Pen"), Icon::FreeDraw},
        {Item::Highlighter, "highlighter",
         QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Highlight"), Icon::Highlight},
        {Item::Spotlight, "spotlight",
         QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Spotlight"), Icon::Spotlight},
        {Item::Text, "text", QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Text"),
         Icon::Text},
        {Item::SerialNumber, "serial-number",
         QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Serial number"),
         Icon::SerialNumber},
        {Item::Filter, "filter", QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Filter"),
         Icon::Filter},
        {Item::Eraser, "eraser", QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Eraser"),
         Icon::Eraser},
        {Item::Watermark, "watermark",
         QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Watermark"), Icon::Watermark},
    };
    return value;
}

[[nodiscard]] inline const Descriptor& descriptor(Item item) {
    for (const Descriptor& candidate : descriptors()) {
        if (candidate.item == item) {
            return candidate;
        }
    }
    return descriptors().constFirst();
}

[[nodiscard]] inline const Descriptor* descriptor(const QString& id) {
    for (const Descriptor& candidate : descriptors()) {
        if (id == QLatin1String(candidate.id)) {
            return &candidate;
        }
    }
    return nullptr;
}

[[nodiscard]] inline QStringList defaultOrder() {
    QStringList result;
    result.reserve(descriptors().size());
    for (const Descriptor& candidate : descriptors()) {
        result.push_back(QString::fromLatin1(candidate.id));
    }
    return result;
}

[[nodiscard]] inline const QVector<EditorDescriptor>& actionDescriptors() {
    static const QVector<EditorDescriptor> value{
        {"barcode-recognition", "ScreenshotToolbarEditorSettingsWidget",
         QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Barcode recognition"),
         Icon::BarcodeRecognition},
        {"table-recognition", "ScreenshotToolbarEditorSettingsWidget",
         QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Table recognition"),
         Icon::TableRecognition},
        {"convert-to-markdown", "ScreenshotToolbarEditorSettingsWidget",
         QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Convert to Markdown"),
         Icon::Markdown},
        {"convert-to-html", "ScreenshotToolbarEditorSettingsWidget",
         QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Convert to HTML"), Icon::Html},
        {"record-screen", "ScreenshotToolbarEditorSettingsWidget",
         QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Record screen"),
         Icon::RecordScreen},
        {"pin-to-screen", "ScreenshotToolbarEditorSettingsWidget",
         QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Pin to screen"),
         Icon::PinToScreen},
        {"text-recognition", "ScreenshotToolbarEditorSettingsWidget",
         QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Text recognition"),
         Icon::TextRecognition},
        {"text-translation", "ScreenshotToolbarEditorSettingsWidget",
         QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Text translation"),
         Icon::TextTranslation},
        {"scrolling-screenshot", "ScreenshotToolbarEditorSettingsWidget",
         QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Scrolling screenshot"),
         Icon::ScrollingScreenshot},
        {"save-as-file", "ScreenshotToolbarEditorSettingsWidget",
         QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Save as file"),
         Icon::SaveAsFile},
    };
    return value;
}

[[nodiscard]] inline QVector<EditorDescriptor>
editorDescriptors(storage::ScreenshotToolbarLayoutKind kind) {
    if (kind == storage::ScreenshotToolbarLayoutKind::ActionTools) {
        return actionDescriptors();
    }
    QVector<EditorDescriptor> result;
    result.reserve(descriptors().size());
    for (const Descriptor& descriptor : descriptors()) {
        result.push_back({descriptor.id, "DrawingToolbarEditorSettingsWidget", descriptor.label,
                          descriptor.icon});
    }
    return result;
}

[[nodiscard]] inline const EditorDescriptor* actionDescriptor(const QString& id) {
    for (const EditorDescriptor& candidate : actionDescriptors()) {
        if (id == QLatin1String(candidate.id)) {
            return &candidate;
        }
    }
    return nullptr;
}

[[nodiscard]] inline QVector<QStringList> defaultPositions() {
    return {
        {QStringLiteral("shape")},     {QStringLiteral("line"), QStringLiteral("arrow")},
        {QStringLiteral("free-draw")}, {QStringLiteral("spotlight"), QStringLiteral("highlighter")},
        {QStringLiteral("text")},      {QStringLiteral("serial-number")},
        {QStringLiteral("filter")},    {QStringLiteral("eraser")},
        {QStringLiteral("watermark")},
    };
}

[[nodiscard]] inline QVector<QStringList> actionDefaultPositions() {
    return {
        {QStringLiteral("table-recognition"), QStringLiteral("barcode-recognition"),
         QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html")},
        {QStringLiteral("record-screen")},
        {QStringLiteral("pin-to-screen")},
        {QStringLiteral("text-recognition")},
        {QStringLiteral("text-translation")},
        {QStringLiteral("scrolling-screenshot")},
        {QStringLiteral("save-as-file")},
    };
}

[[nodiscard]] inline QVector<QStringList>
defaultPositions(storage::ScreenshotToolbarLayoutKind kind) {
    return kind == storage::ScreenshotToolbarLayoutKind::ActionTools ? actionDefaultPositions()
                                                                     : defaultPositions();
}

[[nodiscard]] inline QStringList defaultOrder(storage::ScreenshotToolbarLayoutKind kind) {
    QStringList result;
    const QVector<EditorDescriptor> definitions = editorDescriptors(kind);
    result.reserve(definitions.size());
    for (const EditorDescriptor& descriptor : definitions) {
        result.push_back(QString::fromLatin1(descriptor.id));
    }
    return result;
}

[[nodiscard]] inline storage::ScreenshotToolbarLayout
normalizedLayout(const storage::ScreenshotToolbarLayout& input, const QStringList& defaults,
                 const QVector<QStringList>& defaultLayout) {
    const QSet<QString> known(defaults.cbegin(), defaults.cend());
    QSet<QString> positioned;
    storage::ScreenshotToolbarLayout result;
    for (const QStringList& inputPosition : input.positions) {
        QStringList position;
        for (const QString& itemId : inputPosition) {
            if (known.contains(itemId) && !positioned.contains(itemId)) {
                position.push_back(itemId);
                positioned.insert(itemId);
            }
        }
        if (!position.isEmpty()) {
            result.positions.push_back(position);
        }
    }

    QSet<QString> hidden;
    for (const QString& itemId : input.hidden) {
        if (known.contains(itemId) && !positioned.contains(itemId) && !hidden.contains(itemId)) {
            result.hidden.push_back(itemId);
            hidden.insert(itemId);
        }
    }

    if (!result.positions.isEmpty() && known.contains(QStringLiteral("convert-to-markdown"))) {
        // Fold the previous default's standalone conversions without changing custom placements.
        auto previousDefault = defaultLayout;
        previousDefault[0] = {QStringLiteral("barcode-recognition"),
                              QStringLiteral("table-recognition")};
        previousDefault.insert(1, QStringList{QStringLiteral("convert-to-markdown")});
        previousDefault.insert(2, QStringList{QStringLiteral("convert-to-html")});
        if (result.hidden.isEmpty() && result.positions == previousDefault) {
            result.positions = defaultLayout;
        }
        qsizetype recognitionPosition = -1;
        for (const QString& anchor :
             {QStringLiteral("barcode-recognition"), QStringLiteral("table-recognition")}) {
            for (qsizetype index = 0; index < result.positions.size(); ++index) {
                if (result.positions.at(index).contains(anchor)) {
                    recognitionPosition = index;
                    break;
                }
            }
            if (recognitionPosition >= 0) {
                break;
            }
        }
        for (const QString& itemId :
             {QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html")}) {
            if (recognitionPosition >= 0 && !positioned.contains(itemId) &&
                !hidden.contains(itemId)) {
                result.positions[recognitionPosition].push_back(itemId);
                positioned.insert(itemId);
            }
        }
    }
    for (const QStringList& defaultPosition : defaultLayout) {
        QStringList missing;
        for (const QString& itemId : defaultPosition) {
            if (!positioned.contains(itemId) && !hidden.contains(itemId)) {
                missing.push_back(itemId);
                positioned.insert(itemId);
            }
        }
        if (!missing.isEmpty()) {
            result.positions.push_back(missing);
        }
    }
    return result;
}

[[nodiscard]] inline storage::ScreenshotToolbarLayout
normalizedLayout(const storage::ScreenshotToolbarLayout& input) {
    return normalizedLayout(input, defaultOrder(), defaultPositions());
}

[[nodiscard]] inline storage::ScreenshotToolbarLayout
normalizedLayout(const storage::ScreenshotToolbarLayout& input,
                 storage::ScreenshotToolbarLayoutKind kind) {
    return normalizedLayout(input, defaultOrder(kind), defaultPositions(kind));
}

namespace detail {
struct ItemLocation {
    int positionIndex = -1;
    int itemIndex = -1;
    int hiddenIndex = -1;
};

[[nodiscard]] inline ItemLocation itemLocation(const storage::ScreenshotToolbarLayout& layout,
                                               const QString& itemId) {
    ItemLocation result;
    for (int positionIndex = 0; positionIndex < layout.positions.size(); ++positionIndex) {
        const int itemIndex = layout.positions.at(positionIndex).indexOf(itemId);
        if (itemIndex >= 0) {
            result.positionIndex = positionIndex;
            result.itemIndex = itemIndex;
            return result;
        }
    }
    result.hiddenIndex = layout.hidden.indexOf(itemId);
    return result;
}
} // namespace detail

[[nodiscard]] inline storage::ScreenshotToolbarLayout
moveItemToPosition(const storage::ScreenshotToolbarLayout& input,
                   storage::ScreenshotToolbarLayoutKind kind, const QString& itemId,
                   int targetPositionIndex) {
    storage::ScreenshotToolbarLayout result = normalizedLayout(input, kind);
    const detail::ItemLocation source = detail::itemLocation(result, itemId);
    if (source.positionIndex < 0 && source.hiddenIndex < 0) {
        return result;
    }
    if (source.hiddenIndex >= 0) {
        result.hidden.removeAt(source.hiddenIndex);
    }
    if (source.positionIndex >= 0) {
        result.positions[source.positionIndex].removeAt(source.itemIndex);
        if (result.positions.at(source.positionIndex).isEmpty()) {
            result.positions.removeAt(source.positionIndex);
            if (source.positionIndex < targetPositionIndex) {
                --targetPositionIndex;
            }
        }
    }
    targetPositionIndex =
        std::clamp(targetPositionIndex, 0, static_cast<int>(result.positions.size()));
    result.positions.insert(targetPositionIndex, QStringList{itemId});
    return result;
}

[[nodiscard]] inline storage::ScreenshotToolbarLayout
stackItemInPosition(const storage::ScreenshotToolbarLayout& input,
                    storage::ScreenshotToolbarLayoutKind kind, const QString& itemId,
                    int targetPositionIndex, int targetItemIndex) {
    storage::ScreenshotToolbarLayout result = normalizedLayout(input, kind);
    const detail::ItemLocation source = detail::itemLocation(result, itemId);
    if (source.positionIndex < 0 && source.hiddenIndex < 0) {
        return result;
    }
    if (source.hiddenIndex >= 0) {
        result.hidden.removeAt(source.hiddenIndex);
    }
    if (result.positions.isEmpty()) {
        result.positions.push_back({itemId});
        return result;
    }

    targetPositionIndex =
        std::clamp(targetPositionIndex, 0, static_cast<int>(result.positions.size()) - 1);
    if (source.positionIndex == targetPositionIndex) {
        result.positions[source.positionIndex].removeAt(source.itemIndex);
        if (source.itemIndex < targetItemIndex) {
            --targetItemIndex;
        }
    } else if (source.positionIndex >= 0) {
        result.positions[source.positionIndex].removeAt(source.itemIndex);
        if (result.positions.at(source.positionIndex).isEmpty()) {
            result.positions.removeAt(source.positionIndex);
            if (source.positionIndex < targetPositionIndex) {
                --targetPositionIndex;
            }
        }
    }
    targetItemIndex = std::clamp(targetItemIndex, 0,
                                 static_cast<int>(result.positions.at(targetPositionIndex).size()));
    result.positions[targetPositionIndex].insert(targetItemIndex, itemId);
    return result;
}

[[nodiscard]] inline storage::ScreenshotToolbarLayout
moveItemToHidden(const storage::ScreenshotToolbarLayout& input,
                 storage::ScreenshotToolbarLayoutKind kind, const QString& itemId,
                 int targetHiddenIndex) {
    storage::ScreenshotToolbarLayout result = normalizedLayout(input, kind);
    const detail::ItemLocation source = detail::itemLocation(result, itemId);
    if (source.positionIndex < 0 && source.hiddenIndex < 0) {
        return result;
    }
    if (source.positionIndex >= 0) {
        result.positions[source.positionIndex].removeAt(source.itemIndex);
        if (result.positions.at(source.positionIndex).isEmpty()) {
            result.positions.removeAt(source.positionIndex);
        }
    }
    if (source.hiddenIndex >= 0) {
        result.hidden.removeAt(source.hiddenIndex);
        if (source.hiddenIndex < targetHiddenIndex) {
            --targetHiddenIndex;
        }
    }
    targetHiddenIndex = std::clamp(targetHiddenIndex, 0, static_cast<int>(result.hidden.size()));
    result.hidden.insert(targetHiddenIndex, itemId);
    return result;
}

[[nodiscard]] inline adqt::icons::IconRef icon(Icon semantic) {
    namespace custom = snow_shot::presentation::icons::custom::outlined;
    switch (semantic) {
    case Icon::Shape:
        return custom::ToolRectangle();
    case Icon::Arrow:
        return custom::ToolArrow();
    case Icon::Line:
        return custom::ToolLine();
    case Icon::FreeDraw:
        return custom::ToolFreeDraw();
    case Icon::Highlight:
        return custom::ToolHighlight();
    case Icon::Spotlight:
        return custom::ToolSpotlight();
    case Icon::Text:
        return custom::ToolText();
    case Icon::SerialNumber:
        return custom::ToolSerialNumber();
    case Icon::Filter:
        return custom::ToolFilter();
    case Icon::Eraser:
        return custom::ToolEraser();
    case Icon::Watermark:
        return custom::ToolWatermark();
    case Icon::BarcodeRecognition:
        return custom::ScanQrcode();
    case Icon::TableRecognition:
        return custom::TableRecognition();
    case Icon::Markdown:
        return custom::Markdown();
    case Icon::Html:
        return custom::Html();
    case Icon::RecordScreen:
        return custom::RecordScreen();
    case Icon::PinToScreen:
        return custom::PinToScreen();
    case Icon::TextRecognition:
        return custom::ToolRecognizeText();
    case Icon::TextTranslation:
        return custom::OcrTranslate();
    case Icon::ScrollingScreenshot:
        return custom::ScrollingScreenshot();
    case Icon::SaveAsFile:
        return custom::Save();
    }
    return {};
}

} // namespace snow_shot::presentation::toolbar_layout

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARLAYOUTMODEL_H
