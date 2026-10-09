#include "snow_canvas_text.h"
#include "snow_canvas_text_layout.h"
#include "snow_canvas_text_measurement.h"
#include "snow_canvas_text_render.h"
#include "snow_canvas_text_editor_session.h"
#include "snow_canvas_text_editor_view.h"
#include "snow_canvas_type_conversions.h"
#include "snow_canvas_runtime_access.h"
#include "snow_canvas_viewport.h"
#include "snow_canvas_ffi_handles.h"
#include "snow_draw_engine_qt/snow_canvas_runtime.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"

#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QPainter>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {
static_assert(sizeof(SnowTextStyle) == 232);
static_assert(offsetof(SnowTextStyle, font_family_utf8) == 100);
static_assert(sizeof(SnowTextElementInfo) == 1264);
static_assert(offsetof(SnowTextElementInfo, text_utf8) == 104);
static_assert(offsetof(SnowTextElementInfo, font_family_utf8) == 1136);
static_assert(sizeof(SnowSceneDisplayItem) == 360);
static_assert(offsetof(SnowSceneDisplayItem, arrow_start_head) == 140);
static_assert(offsetof(SnowSceneDisplayItem, font_size) == 192);
static_assert(offsetof(SnowSceneDisplayItem, text_utf8) == 336);

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void fontRenderingAndCachesFollowEmphasis() {
    const QFont baseFont(QStringLiteral("Arial"));
    SnowTextElementInfo info{};
    info.width = 420;
    info.height = 120;
    info.font_size = 40;
    auto item = snow_canvas_text::defaultPreviewItem(info);
    snow_canvas_text::copyTextToSceneItem(item, QStringLiteral("Bold italic fj Wavy"));
    std::array<QImage, 4> images;
    snow_canvas_text_render::clearRenderCacheForCurrentThread();
    snow_canvas_text_measurement::NaturalTextLayoutCache measurements;
    for (std::size_t variant = 0; variant < images.size(); ++variant) {
        item.text_bold = static_cast<std::uint8_t>(variant & 1);
        item.text_italic = static_cast<std::uint8_t>((variant >> 1) & 1);
        const auto resolved = snow_canvas_text_layout::resolveFont(baseFont, item, 1.0);
        const auto editingFont = snow_canvas_text_layout::fontForItem(baseFont, item, 1.0);
        require(resolved.font.bold() == (item.text_bold != 0) &&
                    resolved.font.italic() == (item.text_italic != 0) &&
                    editingFont.bold() == resolved.font.bold() &&
                    editingFont.italic() == resolved.font.italic(),
                "painting and editor fonts must resolve both emphasis flags");
        const auto layout = snow_canvas_text_layout::createDocumentLayout(
            item, baseFont, 1.0, snow_canvas_text::textFromSceneItem(item), false);
        const auto caret = snow_canvas_text_layout::cursorRectInDocument(layout.textDocument(), 4);
        const auto ranges =
            snow_canvas_text_layout::rangeRectsInDocument(layout.textDocument(), 0, 4);
        const auto expectedWidth =
            QFontMetricsF(editingFont).horizontalAdvance(QStringLiteral("Bold"));
        require(std::abs(caret.x() - expectedWidth) < 0.1 && ranges.size() == 1 &&
                    std::abs(ranges.first().width() - expectedWidth) < 0.1,
                "caret and selection widths must follow styled glyph advances");
        measurements.measure(snow_canvas_text::textFromSceneItem(item), baseFont, item);
        images[variant] = QImage(460, 160, QImage::Format_ARGB32_Premultiplied);
        images[variant].fill(Qt::transparent);
        QPainter painter(&images[variant]);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.translate(20, 20);
        snow_canvas_text_render::drawContents(painter, item, baseFont, QRectF(0, 0, 420, 120), 1.0);
        painter.end();
        const auto stats = snow_canvas_text_render::renderCacheDiagnosticsForCurrentThread();
        require(stats.builds == variant + 1,
                "each emphasis combination must have its own retained layout");
        QImage repeat(460, 160, QImage::Format_ARGB32_Premultiplied);
        repeat.fill(Qt::transparent);
        QPainter repeatedPainter(&repeat);
        repeatedPainter.setRenderHint(QPainter::TextAntialiasing);
        repeatedPainter.translate(20, 20);
        snow_canvas_text_render::drawContents(repeatedPainter, item, baseFont,
                                              QRectF(0, 0, 420, 120), 1.0);
        repeatedPainter.end();
        require(repeat == images[variant], "cached text must match the initial rendered pixels");
        for (std::size_t previous = 0; previous < variant; ++previous)
            require(images[variant] != images[previous],
                    "regular, bold, italic and combined text must render distinct pixels");
    }
    require(measurements.measurementCount() == 4,
            "natural text measurements must distinguish all emphasis combinations");
    QFont inherited = baseFont;
    inherited.setBold(true);
    inherited.setItalic(true);
    item.text_bold = 0;
    item.text_italic = 0;
    const auto regular = snow_canvas_text_layout::resolveFont(inherited, item, 1.0);
    require(!regular.font.bold() && !regular.font.italic(),
            "disabled flags must override inherited font emphasis");
}

void measurementsAndActiveDraftsFollowEmphasis() {
    const QFont font(QStringLiteral("Arial"));
    const QByteArray text("wrapping bold italic text changes glyph metrics");
    SnowCanvasTextStyle canvasStyle;
    canvasStyle.fontFamily = font.family();
    canvasStyle.fontSize = 30;
    auto style = snow_canvas_types::toEngineTextStyle(canvasStyle);
    SnowTextElementInfo infos[2]{};
    for (std::size_t index = 0; index < 2; ++index) {
        auto& info = infos[index];
        info.id = SnowElementId{static_cast<std::uint32_t>(index + 1), 1};
        info.width = 100;
        info.height = 40;
        info.font_size = 30;
        info.auto_resize = static_cast<std::uint8_t>(index == 0);
        info.text_utf8_len = static_cast<std::uint32_t>(text.size());
        std::memcpy(info.text_utf8, text.constData(), static_cast<std::size_t>(text.size()));
    }
    style.bold = 1;
    style.italic = 1;
    const auto measured = snow_canvas_text_measurement::measureAutoResizeLayoutOverrides(
        infos, 2, style, font, SNOW_TEXT_STYLE_MIXED_BOLD);
    require(measured.success && measured.layouts.size() == 2,
            "bold edits must remeasure automatic and fixed-width text");
    for (std::size_t index = 0; index < 2; ++index) {
        auto item = snow_canvas_text::defaultPreviewItem(infos[index]);
        item.text_bold = 1;
        const auto expected = index == 0 ? snow_canvas_text_layout::measureNaturalTextLayout(
                                               QString::fromUtf8(text), font, item)
                                         : snow_canvas_text_layout::measureWrappedTextLayout(
                                               QString::fromUtf8(text), font, item, 100);
        require(std::abs(measured.layouts[index].size.width - expected.layout.width()) < 0.001 &&
                    std::abs(measured.layouts[index].size.height - expected.layout.height()) <
                        0.001,
                "a bold-only measurement must preserve each element's italic flag and sizing mode");
    }
    style.bold = 0;
    style.italic = 0;
    auto info = snow_canvas_text::newTextInfoAt(QPointF(0, 0), font, style);
    SnowCanvasTextEditorSession session;
    require(session.begin(info, nullptr, font, &style), "begin a regular text draft");
    QInputMethodEvent input;
    input.setCommitString(QStringLiteral("Draft emphasis"));
    require(session.handleInputMethodEvent(&input, font), "insert draft text");
    session.updatePreviewFromState(font);
    const auto regularWidth = session.previewItem()->width;
    style.bold = 1;
    style.italic = 1;
    SceneDisplayInfo scene{};
    scene.camera_zoom = 1.0;
    session.applyTextStyle(style, font, scene);
    require(session.isActive(), "emphasis changes must preserve active text editing");
    const auto* preview = session.previewItem();
    require(preview != nullptr && preview->text_bold != 0 && preview->text_italic != 0,
            "the active editor preview must carry both flags");
    const auto expectedLayout = snow_canvas_text_layout::measureNaturalTextLayout(
        session.presentationText(), font, *preview);
    require(preview->width != regularWidth &&
                std::abs(preview->width - expectedLayout.layout.width()) < 0.001 &&
                std::abs(preview->height - expectedLayout.layout.height()) < 0.001,
            "emphasis changes must refresh the active draft's measured geometry");
    const auto inputFont = snow_canvas_text_editor_view::inputMethodFont(*preview, font, scene);
    require(inputFont.bold() && inputFont.italic(), "input methods must use the styled font");
    const auto layout = snow_canvas_text_layout::createDocumentLayout(
        *preview, font, 1.0, session.presentationText(), false);
    require(
        snow_canvas_text_layout::cursorRectInDocument(layout.textDocument(), 4).isValid() &&
            !snow_canvas_text_layout::rangeRectsInDocument(layout.textDocument(), 0, 4).isEmpty(),
        "styled drafts must retain valid caret and selection geometry");
    const auto finished = session.finish(font);
    require(finished.style.bold != 0 && finished.style.italic != 0 &&
                finished.text == QStringLiteral("Draft emphasis"),
            "finishing a draft must preserve text and emphasis");
    require(session.begin(info, nullptr, font, &style), "begin a cancellable styled draft");
    session.cancel();
    require(!session.isActive(), "cancel must discard the styled draft");
}

void documentHistoryAndExportsPreserveEmphasis() {
    SnowCanvasRuntime runtime;
    const auto handle = snow_canvas_runtime::Access::handle(runtime);
    SnowCanvasViewport viewport;
    require(viewport.create(handle, snow_canvas_viewport::defaultEngineConfig()),
            "create viewport");
    SnowCanvasWidget canvas(runtime);
    SnowCanvasTextStyle style;
    style.fontFamily = QStringLiteral("Arial");
    style.fontSize = 40;
    style.bold = true;
    style.italic = true;
    require(canvas.setCanvasTextStyle(style), "set styled text defaults");
    const QByteArray text("Export fj Wavy");
    require(snow_viewport_create_text(handle, viewport.get(), 0, 0, text.constData(),
                                      static_cast<std::uint32_t>(text.size()), 320, 70) == SNOW_OK,
            "create bold italic text");
    const auto saved = runtime.serializeDocumentSession();
    const auto styled = runtime.renderToImage(QRectF(-200, -80, 400, 160), QSize(400, 160), {});
    require(!styled.isNull(), "export styled text");
    SnowCanvasRuntime restored;
    require(restored.restoreDocumentSession(saved), "restore styled text document");
    require(restored.renderToImage(QRectF(-200, -80, 400, 160), QSize(400, 160), {}) == styled,
            "serialized text must export identical emphasis pixels after restore");
    require(runtime.undo() && !runtime.hasDocumentContent(), "undo the styled text creation");
    require(runtime.redo() &&
                runtime.renderToImage(QRectF(-200, -80, 400, 160), QSize(400, 160), {}) == styled,
            "redo must restore styled text and its export");
    SnowElementId id{};
    std::uint8_t hit = 0;
    require(snow_viewport_hit_text(handle, viewport.get(), 0, 0, &id, &hit) == SNOW_OK && hit != 0,
            "locate styled text for a masked edit");
    ScopedChangedViewportList changed;
    require(snow_viewport_select_element_ex(handle, viewport.get(), id, changed.outParam()) ==
                SNOW_OK,
            "select the styled text");
    SnowTextElementInfo info{};
    std::uint32_t count = 0;
    require(snow_viewport_get_selected_text_elements(handle, viewport.get(), &info, 1, &count) ==
                    SNOW_OK &&
                count == 1 && info.bold != 0 && info.italic != 0,
            "selected text must expose persisted emphasis");
    style.bold = false;
    auto requested = snow_canvas_types::toEngineTextStyle(style);
    const auto measured = snow_canvas_text_measurement::measureAutoResizeLayoutOverrides(
        &info, 1, requested, QFont(QStringLiteral("Arial")), SNOW_TEXT_STYLE_MIXED_BOLD);
    require(measured.success &&
                snow_viewport_patch_text_style_ex(
                    handle, viewport.get(), &requested, SNOW_TEXT_STYLE_MIXED_BOLD,
                    measured.layouts.data(), static_cast<std::uint32_t>(measured.layouts.size()),
                    changed.outParam()) == SNOW_OK,
            "disable only bold in the selected text");
    require(snow_viewport_get_selected_text_elements(handle, viewport.get(), &info, 1, &count) ==
                    SNOW_OK &&
                info.bold == 0 && info.italic != 0,
            "the masked edit must preserve italic");
    const auto italic = runtime.renderToImage(QRectF(-200, -80, 400, 160), QSize(400, 160), {});
    require(italic != styled && runtime.undo() &&
                runtime.renderToImage(QRectF(-200, -80, 400, 160), QSize(400, 160), {}) == styled,
            "undo an emphasis edit must restore the original export");
    require(runtime.redo() &&
                runtime.renderToImage(QRectF(-200, -80, 400, 160), QSize(400, 160), {}) == italic,
            "redo an emphasis edit must restore the new export");
}
} // namespace

int main(int argc, char** argv) {
#if defined(Q_OS_WIN)
    if (qEnvironmentVariableIsEmpty("QT_QPA_FONTDIR"))
        qputenv("QT_QPA_FONTDIR", qgetenv("WINDIR") + "/Fonts");
#endif
    QApplication application(argc, argv);
#if defined(Q_OS_WIN)
    for (const auto* name : {"arial.ttf", "arialbd.ttf", "ariali.ttf", "arialbi.ttf"})
        require(QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") +
                                                  QStringLiteral("/Fonts/") +
                                                  QString::fromLatin1(name)) >= 0,
                "load all four Arial faces for offscreen emphasis rendering");
#endif
    fontRenderingAndCachesFollowEmphasis();
    measurementsAndActiveDraftsFollowEmphasis();
    documentHistoryAndExportsPreserveEmphasis();
    return 0;
}
