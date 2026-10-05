#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARLAYOUTMODEL_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARLAYOUTMODEL_H

#include "snow_shot/presentation/editionfeatures.h"

#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "snow_shot/storage/settingsadapters.h"
#include "snow_shot/storage/floatingtoolbarsettings.h"
#include "antd_icons.h"

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <algorithm>

namespace snow_shot::presentation::toolbar_layout {

struct StackPresentation {
    QStringList itemIds;
    QStringList popoverItemIds;

    [[nodiscard]] QString entryItemId() const {
        return popoverItemIds.value(0);
    }
};

template <typename IsAvailable>
[[nodiscard]] StackPresentation stackPresentation(const QStringList& position,
                                                  IsAvailable isAvailable) {
    StackPresentation result;
    for (const QString& itemId : position) {
        if (isAvailable(itemId)) {
            result.itemIds.push_back(itemId);
        }
    }
    // Settings store a vertical stack from top to bottom. The bottom available tool is
    // the initial trigger and the leftmost option in the horizontal popover.
    result.popoverItemIds = result.itemIds;
    std::reverse(result.popoverItemIds.begin(), result.popoverItemIds.end());
    return result;
}

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
    Screenshot,
    ScreenshotDelay,
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
    Undo,
    Redo,
    Separator,
    BarcodeRecognition,
    TableRecognition,
    RecordScreen,
    PinToScreen,
    TextRecognition,
    TextTranslation,
    ScrollingScreenshot,
    Print,
    SaveAsFile,
    UploadToCloud,
    QuickSave,
    Copy,
    Latex,
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
    result.append({QStringLiteral("separator"), QStringLiteral("undo"), QStringLiteral("redo")});
    return result;
}

[[nodiscard]] inline const QVector<EditorDescriptor>& actionDescriptors() {
    static const QVector<EditorDescriptor> value = [] {
        QVector<EditorDescriptor> result{
            {"barcode-recognition", "ScreenshotToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Barcode recognition"),
             Icon::BarcodeRecognition},
            {"table-recognition", "ScreenshotToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Table recognition"),
             Icon::TableRecognition},
            {"convert-to-markdown", "ScreenshotToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Convert to Markdown"),
             Icon::Markdown},
            {"latex-recognition", "ScreenshotToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget",
                               "LaTeX Formula Recognition"),
             Icon::Latex},
            {"convert-to-html", "ScreenshotToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Convert to HTML"),
             Icon::Html},
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
            {"print", "ScreenshotToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Print"), Icon::Print},
            {"quick-save", "ScreenshotToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Quick save"),
             Icon::QuickSave},
            {"upload-to-cloud", "ScreenshotToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Upload to Cloud"),
             Icon::UploadToCloud},
            {"save-as-file", "ScreenshotToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("ScreenshotToolbarEditorSettingsWidget", "Save as file"),
             Icon::SaveAsFile},
            {"copy", "PinnedToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("PinnedToolbarEditorSettingsWidget", "Copy to clipboard"),
             Icon::Copy},
        };
        result.removeIf([](const EditorDescriptor& descriptor) {
            return !editionActionToolAvailable(QString::fromLatin1(descriptor.id));
        });
        return result;
    }();
    return value;
}

[[nodiscard]] inline const QVector<EditorDescriptor>& drawingEditorDescriptors() {
    static const QVector<EditorDescriptor> value = [] {
        QVector<EditorDescriptor> result;
        result.reserve(descriptors().size() + 3);
        for (const Descriptor& descriptor : descriptors()) {
            result.push_back({descriptor.id, "DrawingToolbarEditorSettingsWidget", descriptor.label,
                              descriptor.icon});
        }
        result.push_back(
            {"separator", "DrawingToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Separator Component"),
             Icon::Separator});
        result.push_back({"undo", "DrawingToolbarEditorSettingsWidget",
                          QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Undo"),
                          Icon::Undo});
        result.push_back({"redo", "DrawingToolbarEditorSettingsWidget",
                          QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Redo"),
                          Icon::Redo});
        return result;
    }();
    return value;
}

[[nodiscard]] inline QVector<EditorDescriptor>
editorDescriptors(storage::ScreenshotToolbarLayoutKind kind) {
    if (kind == storage::ScreenshotToolbarLayoutKind::FloatingTools) {
        auto result = actionDescriptors();
        const auto ids = storage::floatingToolbarItemIds();
        result.removeIf([&ids](const EditorDescriptor& item) {
            return !ids.contains(QString::fromLatin1(item.id));
        });
        for (auto& item : result) {
            if (QLatin1String(item.id) == QLatin1String("barcode-recognition")) {
                item.translationContext = "FloatingToolbar";
                item.label = QT_TRANSLATE_NOOP("FloatingToolbar", "QR code recognition");
            } else if (QLatin1String(item.id) == QLatin1String("latex-recognition")) {
                item.translationContext = "FloatingToolbar";
                item.label = QT_TRANSLATE_NOOP("FloatingToolbar", "Formula recognition");
            } else if (QLatin1String(item.id) == QLatin1String("record-screen")) {
                item.translationContext = "FloatingToolbar";
                item.label = QT_TRANSLATE_NOOP("FloatingToolbar", "Screen recording");
            }
        }
        result.prepend({"screenshot-delay", "FloatingToolbar",
                        QT_TRANSLATE_NOOP("FloatingToolbar", "Delay %1 seconds to execute"),
                        Icon::ScreenshotDelay});
        result.prepend({"screenshot", "FloatingToolbar",
                        QT_TRANSLATE_NOOP("FloatingToolbar", "Screenshot"), Icon::Screenshot});
        return result;
    }
    if (kind == storage::ScreenshotToolbarLayoutKind::PinnedActionTools) {
        QVector<EditorDescriptor> result;
        for (const auto& descriptor : actionDescriptors()) {
            const QString id = QString::fromLatin1(descriptor.id);
            if (id == QStringLiteral("barcode-recognition") ||
                id == QStringLiteral("table-recognition") ||
                id == QStringLiteral("latex-recognition") ||
                id == QStringLiteral("convert-to-markdown") ||
                id == QStringLiteral("convert-to-html") ||
                id == QStringLiteral("text-recognition") ||
                id == QStringLiteral("text-translation") || id == QStringLiteral("save-as-file") ||
                id == QStringLiteral("quick-save") || id == QStringLiteral("upload-to-cloud") ||
                id == QStringLiteral("print") || id == QStringLiteral("copy")) {
                result.push_back(descriptor);
            }
        }
        result.push_back(
            {"separator", "DrawingToolbarEditorSettingsWidget",
             QT_TRANSLATE_NOOP("DrawingToolbarEditorSettingsWidget", "Separator Component"),
             Icon::Separator});
        return result;
    }
    if (kind == storage::ScreenshotToolbarLayoutKind::ActionTools) {
        auto result = actionDescriptors();
        result.removeIf([](const EditorDescriptor& descriptor) {
            return QLatin1String(descriptor.id) == QLatin1String("copy");
        });
        return result;
    }
    return drawingEditorDescriptors();
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
        {QStringLiteral("watermark")}, {QStringLiteral("separator")},
        {QStringLiteral("undo")},      {QStringLiteral("redo")},
    };
}

[[nodiscard]] inline QVector<QStringList> editionActionPositions(QVector<QStringList> positions) {
    for (QStringList& position : positions) {
        position.removeIf([](const QString& id) {
            return !editionActionToolAvailable(id) ||
                   (app::edition::isMini && id == QStringLiteral("text-recognition"));
        });
    }
    positions.removeIf([](const QStringList& position) { return position.isEmpty(); });
    return positions;
}

[[nodiscard]] inline QVector<QStringList> actionDefaultPositions() {
    return editionActionPositions({
        {QStringLiteral("convert-to-html"), QStringLiteral("convert-to-markdown"),
         QStringLiteral("latex-recognition"), QStringLiteral("barcode-recognition"),
         QStringLiteral("table-recognition")},
        {QStringLiteral("record-screen")},
        {QStringLiteral("pin-to-screen")},
        {QStringLiteral("text-recognition")},
        {QStringLiteral("text-translation")},
        {QStringLiteral("scrolling-screenshot")},
        {QStringLiteral("upload-to-cloud"), QStringLiteral("print"), QStringLiteral("quick-save"),
         QStringLiteral("save-as-file")},
    });
}

[[nodiscard]] inline QVector<QStringList>
defaultPositions(storage::ScreenshotToolbarLayoutKind kind) {
    if (kind == storage::ScreenshotToolbarLayoutKind::FloatingTools)
        return storage::defaultFloatingToolbarLayout().positions;
    if (kind == storage::ScreenshotToolbarLayoutKind::PinnedActionTools) {
        return editionActionPositions({
            {QStringLiteral("convert-to-html"), QStringLiteral("convert-to-markdown"),
             QStringLiteral("latex-recognition"), QStringLiteral("barcode-recognition"),
             QStringLiteral("table-recognition")},
            {QStringLiteral("text-recognition")},
            {QStringLiteral("text-translation")},
            {QStringLiteral("separator")},
            {QStringLiteral("upload-to-cloud"), QStringLiteral("print"),
             QStringLiteral("quick-save"), QStringLiteral("save-as-file")},
            {QStringLiteral("copy")},
        });
    }
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
                 const QVector<QStringList>& defaultLayout, bool migrateScreenshotLayout = false,
                 bool legacyDefaults = true) {
    const QSet<QString> known(defaults.cbegin(), defaults.cend());
    QSet<QString> positioned;
    storage::ScreenshotToolbarLayout result;
    for (const QStringList& inputPosition : input.positions) {
        QStringList position;
        for (const QString& itemId : inputPosition) {
            if (known.contains(itemId) && !positioned.contains(itemId)) {
                if (itemId == QStringLiteral("separator")) {
                    if (!position.isEmpty()) {
                        result.positions.push_back(position);
                        position.clear();
                    }
                    result.positions.push_back({itemId});
                    positioned.insert(itemId);
                    continue;
                }
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

    if (legacyDefaults && app::edition::isMini &&
        known.contains(QStringLiteral("text-recognition")) &&
        !positioned.contains(QStringLiteral("text-recognition")) &&
        !hidden.contains(QStringLiteral("text-recognition"))) {
        result.hidden.push_back(QStringLiteral("text-recognition"));
        hidden.insert(QStringLiteral("text-recognition"));
    }

    if (known.contains(QStringLiteral("upload-to-cloud")) &&
        !positioned.contains(QStringLiteral("upload-to-cloud")) &&
        !hidden.contains(QStringLiteral("upload-to-cloud"))) {
        for (QStringList& position : result.positions) {
            if (position.contains(QStringLiteral("save-as-file"))) {
                position.prepend(QStringLiteral("upload-to-cloud"));
                positioned.insert(QStringLiteral("upload-to-cloud"));
                break;
            }
        }
        if (!positioned.contains(QStringLiteral("upload-to-cloud")) &&
            hidden.contains(QStringLiteral("save-as-file"))) {
            result.hidden.push_back(QStringLiteral("upload-to-cloud"));
            hidden.insert(QStringLiteral("upload-to-cloud"));
        }
    }
    if (known.contains(QStringLiteral("quick-save")) &&
        !positioned.contains(QStringLiteral("quick-save")) &&
        !hidden.contains(QStringLiteral("quick-save"))) {
        for (QStringList& position : result.positions) {
            const qsizetype saveIndex = position.indexOf(QStringLiteral("save-as-file"));
            if (saveIndex >= 0) {
                position.insert(saveIndex, QStringLiteral("quick-save"));
                positioned.insert(QStringLiteral("quick-save"));
                break;
            }
        }
        if (!positioned.contains(QStringLiteral("quick-save")) &&
            hidden.contains(QStringLiteral("save-as-file"))) {
            result.hidden.push_back(QStringLiteral("quick-save"));
            hidden.insert(QStringLiteral("quick-save"));
        }
    }
    if (known.contains(QStringLiteral("print")) && !positioned.contains(QStringLiteral("print")) &&
        !hidden.contains(QStringLiteral("print"))) {
        for (QStringList& position : result.positions) {
            const qsizetype saveIndex = position.indexOf(QStringLiteral("save-as-file"));
            if (saveIndex >= 0) {
                const qsizetype quickIndex = position.indexOf(QStringLiteral("quick-save"));
                position.insert(quickIndex >= 0 ? std::min(quickIndex, saveIndex) : saveIndex,
                                QStringLiteral("print"));
                positioned.insert(QStringLiteral("print"));
                break;
            }
        }
        if (!positioned.contains(QStringLiteral("print")) &&
            hidden.contains(QStringLiteral("save-as-file"))) {
            result.hidden.push_back(QStringLiteral("print"));
            hidden.insert(QStringLiteral("print"));
        }
    }
    if (migrateScreenshotLayout && !result.positions.isEmpty() &&
        known.contains(QStringLiteral("convert-to-markdown"))) {
        // Upgrade earlier defaults without changing custom placements.
        auto previousDefault = defaultLayout;
        previousDefault[0] = {QStringLiteral("barcode-recognition"),
                              QStringLiteral("table-recognition")};
        previousDefault.insert(1, QStringList{QStringLiteral("convert-to-markdown")});
        previousDefault.insert(2, QStringList{QStringLiteral("convert-to-html")});
        auto previousGroupedDefault = defaultLayout;
        previousGroupedDefault[0] = {
            QStringLiteral("table-recognition"), QStringLiteral("barcode-recognition"),
            QStringLiteral("convert-to-markdown"), QStringLiteral("convert-to-html")};
        if (result.hidden.isEmpty() &&
            (result.positions == previousDefault || result.positions == previousGroupedDefault)) {
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
    // Upgrade the previous default recognition group, preserving custom arrangements.
    for (QStringList& position : result.positions) {
        if (legacyDefaults && position == QStringList{QStringLiteral("convert-to-html"),
                                                      QStringLiteral("latex-recognition"),
                                                      QStringLiteral("convert-to-markdown"),
                                                      QStringLiteral("barcode-recognition"),
                                                      QStringLiteral("table-recognition")}) {
            position.swapItemsAt(1, 2);
        }
    }
    if (legacyDefaults && known.contains(QStringLiteral("latex-recognition")) &&
        !positioned.contains(QStringLiteral("latex-recognition")) &&
        !hidden.contains(QStringLiteral("latex-recognition"))) {
        for (QStringList& position : result.positions) {
            if (position.contains(QStringLiteral("latex-recognition"))) {
                positioned.insert(QStringLiteral("latex-recognition"));
                break;
            }
            const auto index = position.indexOf(QStringLiteral("convert-to-markdown"));
            if (index >= 0) {
                // Popup buttons reverse the saved stack: insert after Markdown to appear to its
                // left.
                position.insert(index + 1, QStringLiteral("latex-recognition"));
                positioned.insert(QStringLiteral("latex-recognition"));
                break;
            }
        }
        if (!positioned.contains(QStringLiteral("latex-recognition")) &&
            hidden.contains(QStringLiteral("convert-to-markdown"))) {
            result.hidden.push_back(QStringLiteral("latex-recognition"));
            hidden.insert(QStringLiteral("latex-recognition"));
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
    return normalizedLayout(input, defaultOrder(storage::ScreenshotToolbarLayoutKind::DrawingTools),
                            defaultPositions());
}

[[nodiscard]] inline storage::ScreenshotToolbarLayout
normalizedLayout(const storage::ScreenshotToolbarLayout& input,
                 storage::ScreenshotToolbarLayoutKind kind) {
    if (kind == storage::ScreenshotToolbarLayoutKind::FloatingTools) {
        auto candidate = input;
        for (const auto& id : storage::defaultFloatingToolbarLayout().hidden) {
            const bool positioned =
                std::any_of(candidate.positions.cbegin(), candidate.positions.cend(),
                            [&id](const QStringList& position) { return position.contains(id); });
            if (!positioned && !candidate.hidden.contains(id))
                candidate.hidden.append(id);
        }
        return normalizedLayout(candidate, defaultOrder(kind), defaultPositions(kind), false,
                                false);
    }
    return normalizedLayout(input, defaultOrder(kind), defaultPositions(kind),
                            kind == storage::ScreenshotToolbarLayoutKind::ActionTools);
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
        const int itemIndex = static_cast<int>(layout.positions.at(positionIndex).indexOf(itemId));
        if (itemIndex >= 0) {
            result.positionIndex = positionIndex;
            result.itemIndex = itemIndex;
            return result;
        }
    }
    result.hiddenIndex = static_cast<int>(layout.hidden.indexOf(itemId));
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
    if (kind == storage::ScreenshotToolbarLayoutKind::DrawingTools &&
        (itemId == QStringLiteral("separator") ||
         result.positions.value(targetPositionIndex).contains(QStringLiteral("separator")))) {
        return moveItemToPosition(result, kind, itemId,
                                  targetPositionIndex + (targetItemIndex > 0 ? 1 : 0));
    }
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
    case Icon::Screenshot:
        return snow_shot::presentation::icons::custom::twotone::ScreenshotFeature();
    case Icon::ScreenshotDelay:
        return custom::ScreenshotDelay();
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
    case Icon::Undo:
        return adqt::icons::antd::outlined::Undo();
    case Icon::Redo:
        return adqt::icons::antd::outlined::Redo();
    case Icon::Separator:
        return {};
    case Icon::BarcodeRecognition:
        return custom::ScanQrcode();
    case Icon::TableRecognition:
        return custom::TableRecognition();
    case Icon::Latex:
        return adqt::icons::antd::outlined::Function();
    case Icon::Markdown:
        return custom::Markdown();
    case Icon::Html:
        return custom::Html();
    case Icon::RecordScreen:
        return custom::RecordScreen();
    case Icon::PinToScreen:
        return custom::PinToScreen();
    case Icon::TextRecognition:
        return custom::TextRecognition();
    case Icon::TextTranslation:
        return custom::OcrTranslate();
    case Icon::ScrollingScreenshot:
        return custom::ScrollingScreenshot();
    case Icon::QuickSave:
        return custom::QuickSave();
    case Icon::Copy:
        return adqt::icons::antd::outlined::Copy();
    case Icon::Print:
        return adqt::icons::antd::outlined::Printer();
    case Icon::UploadToCloud:
        return adqt::icons::antd::outlined::CloudUpload();
    case Icon::SaveAsFile:
        return custom::Save();
    }
    return {};
}

} // namespace snow_shot::presentation::toolbar_layout

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTTOOLBARLAYOUTMODEL_H
