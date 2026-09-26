#include "snow_canvas_renderer.h"
#include "snow_canvas_text.h"
#include "snow_canvas_text_draft.h"
#include "snow_canvas_text_editor_overlay.h"
#include "snow_canvas_text_editor_view.h"
#include "snow_canvas_text_editor_session.h"
#include "snow_canvas_text_layout.h"

#include <QApplication>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>

#include <cstdlib>
#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool sameRendering(const QImage& actual, const QImage& expected);

void restoredDraftRepairsStaleContentBounds() {
    // Geometry from the reported history entry: the font/frame were enlarged
    // 3.1x while the old ink dimensions survived the edit commit.
    SnowTextElementInfo info{};
    info.id = {0, 1};
    info.width = 1353.8434721147723;
    info.height = 287.28952547482;
    info.font_size = 225.94124138905113;
    info.rotation = -0.6444283836632949;
    info.auto_resize = 1;
    SnowCanvasSceneItem item = snow_canvas_text::defaultPreviewItem(info);
    item.content_width = 427.5602218386809;
    item.content_height = 91.8349550858442;
    item.text_horizontal_align = SNOW_TEXT_HORIZONTAL_ALIGN_CENTER;
    item.text_vertical_align = SNOW_TEXT_VERTICAL_ALIGN_CENTER;
    item.fill = {255, 204, 199, 255};
    item.stroke = {217, 247, 190, 255};
    item.stroke_width = 4.0;
    const QString text = QStringLiteral("\u6492\u6253\u53d123231");
    snow_canvas_text::copyTextToSceneItem(item, text);
    const QFont font;
    const auto measured =
        snow_canvas_text_layout::measureWrappedTextLayout(text, font, item, item.width);
    SnowCanvasTextEditorSession session;
    require(session.begin(info, &item, font, nullptr, &text), "open saved resized text");
    const auto* preview = session.previewItem();
    require(preview->width == item.width && preview->height == item.height &&
                preview->rotation == item.rotation,
            "repairing old ink bounds must preserve the saved frame and rotation");
    require(preview->content_width == measured.content.width() &&
                preview->content_height == measured.content.height(),
            "restored text must publish current ink bounds instead of obsolete saved dimensions");
    SnowCanvasSceneItem painted(*preview);
    SceneDisplayInfo scene{};
    scene.surface_width = 1600;
    scene.surface_height = 1000;
    scene.camera_zoom = 0.75;
    SnowCanvasTextDraft draft;
    draft.begin(text);
    draft.selectAll();
    auto paint = [&](QImage& image, const QRegion& region) {
        QPainter painter(&image);
        painter.setFont(font);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setClipRegion(region);
        painter.fillRect(image.rect(), Qt::white);
        snow_canvas_renderer::SceneRenderRequest request;
        request.painter = &painter;
        request.displayInfo = &scene;
        request.sceneItems = &painted;
        request.sceneItemCount = 1;
        request.exposedRegion = region;
        snow_canvas_renderer::renderSceneItems(request);
        snow_canvas_text_editor_view::renderOverlay(painter, painted, draft, font, scene, false);
    };
    QImage complete(1600, 1000, QImage::Format_ARGB32_Premultiplied);
    complete.fill(Qt::white);
    paint(complete, QRegion(complete.rect()));
    // The first and last glyphs lie outside the obsolete pre-resize ink box.
    // Repaint those caret regions to exercise scene culling as in the report.
    auto repaintEnds = [&]() {
        QImage incremental = complete;
        for (int position : {0, static_cast<int>(text.size())}) {
            draft.setCursorPosition(position);
            const QRegion region =
                snow_canvas_text_editor_view::caretRegion(painted, draft, font, scene);
            draft.selectAll();
            paint(incremental, region);
        }
        return incremental;
    };
    painted.content_width = item.content_width;
    painted.content_height = item.content_height;
    require(!sameRendering(repaintEnds(), complete),
            "the reported stale bounds must reproduce missing paint at the text ends");
    painted = *preview;
    require(sameRendering(repaintEnds(), complete),
            "repainting the ends of restored rotated text must not cull the underlying glyphs");
    const auto committed = session.finish(font);
    require(committed.measuredLayout.content_width == measured.content.width() &&
                committed.measuredLayout.content_height == measured.content.height(),
            "the next commit must retain repaired bounds");
}

bool sameRendering(const QImage& actual, const QImage& expected) {
    // Qt rounds clipped antialiased spans at fractional DPR. Compositing the
    // background, outline, highlight and glyphs can accumulate two channel
    // levels of rounding; missing or repeated paint is substantially larger.
    for (int y = 0; y < actual.height(); ++y) {
        const auto* actualRow = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        const auto* expectedRow = reinterpret_cast<const QRgb*>(expected.constScanLine(y));
        for (int x = 0; x < actual.width(); ++x) {
            const QRgb a = actualRow[x];
            const QRgb b = expectedRow[x];
            if (std::abs(qRed(a) - qRed(b)) > 2 || std::abs(qGreen(a) - qGreen(b)) > 2 ||
                std::abs(qBlue(a) - qBlue(b)) > 2 || qAlpha(a) != qAlpha(b)) {
                return false;
            }
        }
    }
    return true;
}

void requireUnchangedOutsideRegion(const QImage& actual, const QImage& before,
                                   const QRegion& region) {
    QImage mask(actual.size(), QImage::Format_ARGB32_Premultiplied);
    mask.setDevicePixelRatio(actual.devicePixelRatio());
    mask.fill(Qt::transparent);
    {
        QPainter painter(&mask);
        painter.setClipRegion(region);
        painter.fillRect(QRect(0, 0, 800, 600), Qt::white);
    }
    for (int y = 0; y < actual.height(); ++y) {
        const auto* actualRow = reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        const auto* beforeRow = reinterpret_cast<const QRgb*>(before.constScanLine(y));
        const auto* maskRow = reinterpret_cast<const QRgb*>(mask.constScanLine(y));
        for (int x = 0; x < actual.width(); ++x) {
            require(qAlpha(maskRow[x]) != 0 || actualRow[x] == beforeRow[x],
                    "text painting must leave pixels outside the exposed region exactly unchanged");
        }
    }
}

void caretInvalidationContainsPaintedStroke() {
    const QFont font(QStringLiteral("Arial"));
    for (double zoom : {0.25, 1.0, 8.0, 16.0}) {
        for (double rotation : {0.0, -0.47, 1.57}) {
            SceneDisplayInfo scene{};
            scene.surface_width = 800;
            scene.surface_height = 600;
            scene.camera_zoom = zoom;
            SnowSceneDisplayItem item{};
            item.width = 300 / zoom;
            item.height = 100 / zoom;
            item.font_size = 40 / zoom;
            item.rotation = rotation;
            item.text_color = {0, 0, 0, 255};
            SnowCanvasTextDraft draft;
            draft.begin(QStringLiteral("ABC"));
            draft.setCursorPosition(1);
            const QRegion region =
                snow_canvas_text_editor_view::caretRegion(item, draft, font, scene);
            QImage image(800, 600, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            {
                QPainter painter(&image);
                painter.setRenderHint(QPainter::Antialiasing);
                snow_canvas_text_editor_overlay::renderCaret(painter, item, draft.text(), 1, font,
                                                             QPointF(400, 300), zoom);
            }
            for (int y = 0; y < image.height(); ++y) {
                for (int x = 0; x < image.width(); ++x) {
                    require(qAlpha(image.pixel(x, y)) == 0 || region.contains(QPoint(x, y)),
                            "caret invalidation must contain its entire stroke at every zoom and "
                            "rotation");
                }
            }
        }
    }
}

void incrementalTextPaintingMatchesFullFrame() {
    const QString text = QStringLiteral("SDF123\nSDF12321321311");
    const QFont font(QStringLiteral("Arial"));
    for (double dpr : {1.0, 1.25, 2.0}) {
        for (double rotation : {0.0, -0.47}) {
            for (double zoom : {1.0, 1.37}) {
                SceneDisplayInfo scene{};
                scene.surface_width = 800;
                scene.surface_height = 600;
                scene.camera_zoom = zoom;
                SnowCanvasSceneItem item;
                item.kind = SNOW_SCENE_DISPLAY_ITEM_TEXT;
                item.width = 420;
                item.height = 160;
                item.font_size = 42.3;
                item.rotation = rotation;
                item.opacity = 1.0;
                item.fill = {255, 180, 180, 160};
                item.fill_style = SNOW_FILL_STYLE_SOLID;
                item.text_color = {244, 33, 44, 255};
                item.stroke = {180, 255, 180, 255};
                item.stroke_width = 2;
                snow_canvas_text::copyTextToSceneItem(item, text);
                SnowCanvasTextDraft draft;
                draft.begin(text);
                const QRegion full(QRect(0, 0, 800, 600));
                auto paint = [&](QImage& image, const QRegion& region, bool caret) {
                    QPainter painter(&image);
                    painter.setFont(font);
                    painter.setRenderHint(QPainter::Antialiasing);
                    painter.setClipRegion(region);
                    painter.fillRect(QRect(0, 0, 800, 600), Qt::white);
                    snow_canvas_renderer::SceneRenderRequest request;
                    request.painter = &painter;
                    request.displayInfo = &scene;
                    request.sceneItems = &item;
                    request.sceneItemCount = 1;
                    request.exposedRegion = region;
                    snow_canvas_renderer::renderSceneItems(request);
                    snow_canvas_text_editor_view::renderOverlay(painter, item, draft, font, scene,
                                                                caret);
                };
                auto frame = [&](bool caret) {
                    QImage image(QSize(qRound(800 * dpr), qRound(600 * dpr)),
                                 QImage::Format_ARGB32_Premultiplied);
                    image.setDevicePixelRatio(dpr);
                    image.fill(Qt::white);
                    paint(image, full, caret);
                    return image;
                };
                for (bool selected : {false, true}) {
                    draft.setCursorPosition(2);
                    if (selected) {
                        draft.setCursorPosition(17, true);
                    }
                    QImage incremental = frame(true);
                    const QRegion caret =
                        snow_canvas_text_editor_view::caretRegion(item, draft, font, scene);
                    const QImage beforeBlink = incremental;
                    paint(incremental, caret, false);
                    requireUnchangedOutsideRegion(incremental, beforeBlink, caret);
                    const QImage expected = frame(false);
                    if (!sameRendering(incremental, expected)) {
                        std::cerr << "dpr=" << dpr << " rotation=" << rotation << " zoom=" << zoom
                                  << " selected=" << selected << '\n';
                        incremental.save(QStringLiteral("text-overlay-incremental.png"));
                        expected.save(QStringLiteral("text-overlay-expected.png"));
                    }
                    require(sameRendering(incremental, expected),
                            "caret-only repaint must reproduce a complete frame without gaps or "
                            "overdraw");
                    paint(incremental, caret, true);
                    require(sameRendering(incremental, frame(true)),
                            "showing the caret must preserve the text and selection underneath");

                    // Replay disjoint dirty rectangles, as happens when the caret moves
                    // or another overlay invalidates only part of a rotated text item.
                    const QRegion sparse =
                        QRegion(QRect(210, 195, 27, 170)) + QRegion(QRect(430, 250, 41, 180));
                    const QImage beforeSparse = incremental;
                    paint(incremental, sparse, true);
                    requireUnchangedOutsideRegion(incremental, beforeSparse, sparse);
                    if (!sameRendering(incremental, frame(true))) {
                        std::cerr << "sparse dpr=" << dpr << " rotation=" << rotation
                                  << " zoom=" << zoom << " selected=" << selected << '\n';
                        incremental.save(QStringLiteral("text-overlay-incremental.png"));
                        frame(true).save(QStringLiteral("text-overlay-expected.png"));
                    }
                    require(sameRendering(incremental, frame(true)),
                            "sparse repaints must preserve pixels outside the exposed region");
                }
            }
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
#if defined(Q_OS_WIN)
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/arial.ttf")) >= 0,
            "text rendering tests require a system TrueType font");
    require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/msyh.ttc")) >= 0,
            "history reproduction requires a CJK font");
#endif
    caretInvalidationContainsPaintedStroke();
    restoredDraftRepairsStaleContentBounds();
    incrementalTextPaintingMatchesFullFrame();
    return 0;
}
