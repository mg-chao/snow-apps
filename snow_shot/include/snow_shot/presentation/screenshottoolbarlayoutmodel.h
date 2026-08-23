#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARLAYOUTMODEL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARLAYOUTMODEL_H

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

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
};

struct Descriptor {
    Item item = Item::Shape;
    const char* id = nullptr;
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
         QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Highlighter Tool"),
         Icon::Highlight},
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

[[nodiscard]] inline QString id(Item item) {
    return QString::fromLatin1(descriptor(item).id);
}

[[nodiscard]] inline QStringList defaultOrder() {
    QStringList result;
    result.reserve(descriptors().size());
    for (const Descriptor& candidate : descriptors()) {
        result.push_back(QString::fromLatin1(candidate.id));
    }
    return result;
}

[[nodiscard]] inline QVector<QStringList> defaultPositions() {
    return {
        {QStringLiteral("shape")},
        {QStringLiteral("line"), QStringLiteral("arrow")},
        {QStringLiteral("free-draw")},
        {QStringLiteral("spotlight"), QStringLiteral("highlighter")},
        {QStringLiteral("text")},
        {QStringLiteral("serial-number")},
        {QStringLiteral("filter")},
        {QStringLiteral("eraser")},
        {QStringLiteral("watermark")},
    };
}

[[nodiscard]] inline storage::ScreenshotToolbarLayout
normalizedLayout(const storage::ScreenshotToolbarLayout& input) {
    const QStringList defaults = defaultOrder();
    const QSet<QString> known(defaults.cbegin(), defaults.cend());
    QSet<QString> positioned;
    storage::ScreenshotToolbarLayout result;
    for (const QStringList& inputPosition : input.positions) {
        QStringList position;
        for (const QString& storedItemId : inputPosition) {
            const QString itemId =
                storedItemId == QStringLiteral("rectangle-highlight") ||
                        storedItemId == QStringLiteral("pen-highlight")
                    ? QStringLiteral("highlighter")
                    : storedItemId;
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
    for (const QString& storedItemId : input.hidden) {
        const QString itemId =
            storedItemId == QStringLiteral("rectangle-highlight") ||
                    storedItemId == QStringLiteral("pen-highlight")
                ? QStringLiteral("highlighter")
                : storedItemId;
        if (known.contains(itemId) && !positioned.contains(itemId) && !hidden.contains(itemId)) {
            result.hidden.push_back(itemId);
            hidden.insert(itemId);
        }
    }

    for (const QStringList& defaultPosition : defaultPositions()) {
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
    }
    return {};
}

} // namespace snow_shot::presentation::toolbar_layout

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARLAYOUTMODEL_H
