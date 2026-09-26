#include "snow_canvas_text.h"

#include "snow_canvas_text_layout.h"
#include "snow_canvas_utf8.h"

#include <QSizeF>

#include <cmath>
#include <cstddef>
#include <cstring>

namespace snow_canvas_text {
SnowTextStyle patchedTextStyle(SnowTextStyle current, const SnowTextStyle& requested,
                               std::uint32_t properties) {
    if (properties & SNOW_TEXT_STYLE_MIXED_COLOR) {
        current.color = requested.color;
    }
    if (properties & SNOW_TEXT_STYLE_MIXED_FONT_SIZE) {
        current.font_size = requested.font_size;
    }
    if (properties & SNOW_TEXT_STYLE_MIXED_FONT_FAMILY) {
        std::memcpy(current.font_family_utf8, requested.font_family_utf8,
                    sizeof(current.font_family_utf8));
        current.font_family_utf8_len = requested.font_family_utf8_len;
        current.font_family_truncated = requested.font_family_truncated;
    }
    if (properties & SNOW_TEXT_STYLE_MIXED_FILL) {
        current.fill = requested.fill;
    }
    if (properties & SNOW_TEXT_STYLE_MIXED_FILL_STYLE) {
        current.fill_style = requested.fill_style;
    }
    if (properties & SNOW_TEXT_STYLE_MIXED_STROKE) {
        current.stroke = requested.stroke;
    }
    if (properties & SNOW_TEXT_STYLE_MIXED_STROKE_WIDTH) {
        current.stroke_width = requested.stroke_width;
    }
    if (properties & SNOW_TEXT_STYLE_MIXED_CORNER_RADII) {
        current.corner_radii = requested.corner_radii;
    }
    if (properties & SNOW_TEXT_STYLE_MIXED_HORIZONTAL_ALIGN) {
        current.horizontal_align = requested.horizontal_align;
    }
    if (properties & SNOW_TEXT_STYLE_MIXED_VERTICAL_ALIGN) {
        current.vertical_align = requested.vertical_align;
    }
    if (properties & SNOW_TEXT_STYLE_MIXED_OPACITY) {
        current.opacity = requested.opacity;
    }
    return current;
}

namespace text_layout = snow_canvas_text_layout;
namespace {

constexpr double kDefaultTextFontSize = 30.0;
constexpr double kMinimumTextFontSize = 6.0;

template <std::size_t Capacity>
QString stringFromUtf8Field(const char (&bytes)[Capacity], std::uint32_t length) {
    return snow_canvas_utf8::stringFromField(bytes, length, static_cast<std::uint32_t>(Capacity));
}

template <std::size_t Capacity>
void writeStringField(char (&destination)[Capacity], std::uint32_t& destinationLength,
                      std::uint8_t& destinationTruncated, const QString& text,
                      bool sourceTruncated = false) {
    snow_canvas_utf8::copyStringToField(text, destination, destinationLength, destinationTruncated,
                                        static_cast<std::uint32_t>(Capacity));
    if (sourceTruncated) {
        destinationTruncated = 1;
    }
}

QString stringFromUtf8View(const char* bytes, std::uint32_t length) {
    if (bytes == nullptr || length == 0) {
        return {};
    }
    return QString::fromUtf8(bytes, static_cast<qsizetype>(length));
}

void copyFontFamilyToSceneItem(SnowCanvasSceneItem& item, const SnowTextStyle& style) {
    item.setFontFamilyUtf8(
        stringFromUtf8Field(style.font_family_utf8, style.font_family_utf8_len).trimmed().toUtf8());
}

void copyFontFamilyStringToSceneItem(SnowCanvasSceneItem& item, const QString& family) {
    item.setFontFamilyUtf8(family.toUtf8());
}

} // namespace

double defaultTextFontSize() {
    return kDefaultTextFontSize;
}

double minimumTextFontSize() {
    return kMinimumTextFontSize;
}

double resolvedTextFontSize(double fontSize) {
    if (!std::isfinite(fontSize) || fontSize <= 0.0) {
        return kDefaultTextFontSize;
    }
    return qMax(kMinimumTextFontSize, fontSize);
}

QString textFromSceneItem(const SnowSceneDisplayItem& item) {
    return stringFromUtf8View(item.text_utf8, item.text_utf8_len);
}

QString textFromElementInfo(const SnowTextElementInfo& info) {
    return stringFromUtf8Field(info.text_utf8, info.text_utf8_len);
}

QString fontFamilyFromSceneItem(const SnowSceneDisplayItem& item) {
    return stringFromUtf8View(item.font_family_utf8, item.font_family_utf8_len).trimmed();
}

void copyTextToSceneItem(SnowCanvasSceneItem& item, const QString& text) {
    item.setTextUtf8(text.toUtf8());
}

void applyTextStyleToSceneItem(SnowCanvasSceneItem& item, const SnowTextStyle& style) {
    item.text_color = style.color;
    item.font_size = resolvedTextFontSize(style.font_size);
    item.fill = style.fill;
    item.fill_style = style.fill_style;
    item.stroke = style.stroke;
    item.stroke_width = style.stroke_width;
    item.corner_radii = style.corner_radii;
    item.text_horizontal_align = style.horizontal_align;
    item.text_vertical_align = style.vertical_align;
    item.opacity = style.opacity;
    copyFontFamilyToSceneItem(item, style);
}

SnowTextStyle textStyleFromSceneItem(const SnowSceneDisplayItem& item) {
    SnowTextStyle style{};
    style.color = item.text_color;
    style.font_size = resolvedTextFontSize(item.font_size);
    style.fill = item.fill;
    style.fill_style = item.fill_style;
    style.stroke = item.stroke;
    style.stroke_width = item.stroke_width;
    style.corner_radii = item.corner_radii;
    style.horizontal_align = item.text_horizontal_align;
    style.vertical_align = item.text_vertical_align;
    style.opacity = item.opacity;

    writeStringField(style.font_family_utf8, style.font_family_utf8_len,
                     style.font_family_truncated, fontFamilyFromSceneItem(item));
    return style;
}

SnowTextElementInfo newTextInfoAt(const QPointF& canvasPoint, const QFont& baseFont,
                                  const SnowTextStyle& style) {
    SnowTextElementInfo info{};
    info.center_x = canvasPoint.x();
    info.center_y = canvasPoint.y();
    info.font_size = resolvedTextFontSize(style.font_size);
    info.auto_resize = 1;
    SnowCanvasSceneItem item = defaultPreviewItem(info);
    applyTextStyleToSceneItem(item, style);
    const QSizeF initialSize = text_layout::measureNaturalText(QString(), baseFont, item);
    info.width = initialSize.width();
    info.height = initialSize.height();
    return info;
}

SnowCanvasSceneItem defaultPreviewItem(const SnowTextElementInfo& info) {
    SnowCanvasSceneItem item;
    item.kind = SNOW_SCENE_DISPLAY_ITEM_TEXT;
    item.element_id = info.id;
    item.center_x = info.center_x;
    item.center_y = info.center_y;
    item.width = qMax(1.0, info.width);
    item.height = qMax(1.0, info.height);
    item.content_width = info.content_width;
    item.content_height = info.content_height;
    item.rotation = info.rotation;
    item.text_color = SnowColorRgba8{0xf4, 0x21, 0x2c, 0xff};
    item.fill = SnowColorRgba8{0, 0, 0, 0};
    item.stroke = SnowColorRgba8{0xff, 0xcc, 0xc7, 0xff};
    item.stroke_width = 0.0;
    item.corner_radii = SnowCornerRadii{6.0, 6.0, 6.0, 6.0};
    item.font_size = resolvedTextFontSize(info.font_size);
    item.opacity = 1.0;
    item.text_horizontal_align = SNOW_TEXT_HORIZONTAL_ALIGN_LEFT;
    item.text_vertical_align = SNOW_TEXT_VERTICAL_ALIGN_CENTER;
    item.fill_style = SNOW_FILL_STYLE_SOLID;
    item.stroke_style = SNOW_STROKE_STYLE_SOLID;
    copyTextToSceneItem(item, textFromElementInfo(info));
    if (info.font_family_utf8_len > 0) {
        const QString family =
            stringFromUtf8Field(info.font_family_utf8, info.font_family_utf8_len).trimmed();
        if (!family.isEmpty()) {
            copyFontFamilyStringToSceneItem(item, family);
        }
    }
    return item;
}

void updatePreviewFromEditorText(SnowCanvasSceneItem& item, const QString& text, bool autoResize,
                                 const QFont& baseFont) {
    copyTextToSceneItem(item, text);
    if (autoResize) {
        const text_layout::TextMeasuredLayout measured =
            text_layout::measureNaturalTextLayout(text, baseFont, item);
        item.width = measured.layout.width();
        item.height = measured.layout.height();
        item.content_width = measured.content.width();
        item.content_height = measured.content.height();
    } else {
        const text_layout::TextMeasuredLayout measured =
            text_layout::measureWrappedTextLayout(text, baseFont, item, item.width);
        item.height = measured.layout.height();
        item.content_width = measured.content.width();
        item.content_height = measured.content.height();
    }
}

} // namespace snow_canvas_text
