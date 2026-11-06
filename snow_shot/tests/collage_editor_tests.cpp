// SPDX-License-Identifier: GPL-3.0-only
#include "snow_shot/presentation/components/collageeditorwidget.h"
#include "snow_shot/presentation/components/overlaycontrolbutton.h"
#include "snow_shot/presentation/screenshotexportcoordinator.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_draw_engine_qt/snow_canvas_overlay_style.h"
#include "theme/theme_manager.h"
#include "widgets/button.h"
#include "widgets/color_picker.h"
#include "widgets/divider.h"
#include "widgets/input_number.h"
#include "widgets/segmented.h"
#include "widgets/scroll_area.h"

#include <QApplication>
#include <QColorSpace>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QRgba64>
#include <QDir>
#include <QFileInfo>
#include <QThread>
#include <QTemporaryDir>
#include <QTranslator>
#include <QWheelEvent>
#include <QUrl>
#include <QtTest/qtestspontaneevent.h>
#include <QtMath>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace snow_shot::presentation;
using namespace snow_shot::presentation::components;
using adqt::widgets::AdButton;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TemporaryApplicationStorage final {
  public:
    TemporaryApplicationStorage() {
        require(m_directory.isValid() &&
                    snow_shot::storage::ApplicationStorage::instance()
                        .initialize({m_directory.path(), m_directory.path(), 60000})
                        .success,
                "initialize isolated editor test storage without accessing user settings");
    }
    ~TemporaryApplicationStorage() {
        ScreenshotExportCoordinator::shared().shutdown();
        snow_shot::storage::ApplicationStorage::instance().shutdown();
    }

  private:
    QTemporaryDir m_directory;
};
QImage image(QSize size, QColor color = Qt::red) {
    QImage result(size, QImage::Format_ARGB32_Premultiplied);
    result.fill(color);
    result.setColorSpace(QColorSpace::SRgb);
    return result;
}
void pump() {
    QApplication::processEvents();
}
void mouse(QWidget* widget, QEvent::Type type, QPointF point, Qt::MouseButton button,
           Qt::MouseButtons buttons) {
    QMouseEvent event(type, point, widget->mapToGlobal(point.toPoint()), button, buttons,
                      Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}
void wheel(QWidget* widget, QPointF point, int delta) {
    QWheelEvent event(point, widget->mapToGlobal(point.toPoint()), {}, QPoint(0, delta),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(widget, &event);
}
void click(QWidget* widget) {
    const auto center = QRectF(widget->rect()).center();
    mouse(widget, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    mouse(widget, QEvent::MouseButtonRelease, center, Qt::LeftButton, Qt::NoButton);
    pump();
}
AdButton* button(QWidget* parent, const char* name) {
    auto* result = parent->findChild<AdButton*>(QString::fromLatin1(name));
    require(result != nullptr, "expected editor button is missing");
    return result;
}

void externalImageDrops() {
    ImageStitchingDocument document;
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 650);
    editor.show();
    pump();
    auto* hint = editor.findChild<QWidget*>(QStringLiteral("collageDropHint"));
    auto* empty = editor.findChild<QWidget*>(QStringLiteral("collageEmptyState"));
    require(hint && empty && hint->isHidden(), "drop hint starts hidden");
    QStringList received;
    int drops = 0;
    QObject::connect(&editor, &ImageStitchingEditorWidget::imageFilesDropped, &editor,
                     [&](const QStringList& paths) {
                         received = paths;
                         ++drops;
                     });
    QMimeData files;
    const auto first = QDir::temp().filePath(QStringLiteral("collage-first.PNG"));
    const auto second = QDir::temp().filePath(QStringLiteral("collage-second.jpg"));
    const auto bitmapPath = QDir::temp().filePath(QStringLiteral("collage-bitmap.BMP"));
    files.setUrls({QUrl::fromLocalFile(first), QUrl::fromLocalFile(first),
                   QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("notes.txt"))),
                   QUrl(QStringLiteral("https://example.com/image.png")),
                   QUrl::fromLocalFile(second), QUrl::fromLocalFile(bitmapPath)});
    const auto enter = [&](QWidget* target, const QMimeData* mime,
                           Qt::DropActions actions = Qt::CopyAction | Qt::MoveAction) {
        QDragEnterEvent event(QRectF(target->rect()).center(), actions, mime, Qt::LeftButton,
                              Qt::NoModifier);
        QApplication::sendEvent(target, &event);
        return event.isAccepted() && event.dropAction() == Qt::CopyAction;
    };
    const auto leave = [&] {
        QDragLeaveEvent event;
        QApplication::sendEvent(&editor, &event);
        require(hint->isHidden(), "leaving the editor clears drag feedback");
    };
    require(enter(empty, &files) && hint->isVisible() && hint->geometry() == empty->geometry(),
            "external files propagate from the empty state and highlight its preview area");
    require(drops == 0 && document.items().isEmpty(), "hover never imports or edits images");
    leave();
    QMimeData textOnly;
    textOnly.setText(QStringLiteral("text is not an image"));
    QMimeData invalid;
    invalid.setUrls({QUrl(QStringLiteral("https://example.com/image.png")),
                     QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("notes.txt")))});
    QMimeData bitmap;
    bitmap.setImageData(image({2, 2}));
    for (const auto* mime : {&textOnly, &invalid, &bitmap}) {
        require(!enter(&editor, mime) && hint->isHidden(),
                "unsupported payloads do not show a file import hint");
    }
    require(!enter(&editor, &files, Qt::MoveAction), "file imports require copy permission");
    require(enter(&editor, &files), "image files can enter the editor");
    QDragMoveEvent move(QRectF(editor.rect()).center(), Qt::CopyAction | Qt::MoveAction, &files,
                        Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&editor, &move);
    require(move.isAccepted() && move.dropAction() == Qt::CopyAction,
            "moving over the editor preserves copy semantics");
    QDropEvent drop(editor.rect().center(), Qt::CopyAction | Qt::MoveAction, &files, Qt::LeftButton,
                    Qt::NoModifier);
    QApplication::sendEvent(&editor, &drop);
    require(drop.isAccepted() && drop.dropAction() == Qt::CopyAction && drops == 1 &&
                received == QStringList{first, first, second, bitmapPath} && hint->isHidden(),
            "drop emits only local image files, preserving order and duplicates");

    const auto id = document.append(image({120, 80}), QStringLiteral("existing"));
    document.append(image({80, 80}, Qt::blue), QStringLiteral("second"));
    pump();
    for (auto* target : {static_cast<QWidget*>(editor.canvas()),
                         editor.findChild<QWidget*>(QStringLiteral("collageThumbnail_%1").arg(id)),
                         button(&editor, "collageAddImages")->parentWidget(),
                         editor.findChild<QWidget*>(QStringLiteral("collageSettingsScroll"))}) {
        require(target && enter(target, &files) && hint->isVisible(),
                "external image drops work over the canvas, thumbnails, actions and settings");
        leave();
    }
    require(enter(editor.canvas(), &files), "populated preview accepts external files");
    editor.resize(1100, 720);
    pump();
    require(hint->geometry() == editor.canvas()->geometry(),
            "active hint follows the resized preview without covering settings");
    for (const auto theme : {adqt::theme::ThemeScheme::Dark, adqt::theme::ThemeScheme::Light}) {
        adqt::theme::ThemeManager::instance().setPreset(theme);
        pump();
        const auto tokens = adqt::theme::ThemeManager::instance().resolveTheme(hint);
        const auto pixels = hint->grab().toImage();
        require(pixels.pixelColor(pixels.width() / 2, 12) == tokens.colorPrimaryBg,
                "active surface uses Ant Design's primary background in both themes");
    }
    QTranslator translator;
    require(translator.load(QStringLiteral(":/i18n/snow_shot_zh_CN.qm")),
            "drop hint translations are available");
    QApplication::installTranslator(&translator);
    pump();
    const auto title =
        QCoreApplication::translate("ImageStitchingEditorWidget", "Drop images to stitch");
    require(title != QStringLiteral("Drop images to stitch") && hint->accessibleName() == title &&
                hint->findChild<QLabel*>(QStringLiteral("collageDropTitle"))->text() == title,
            "visible drop hint and accessibility retranslate immediately");
    QApplication::removeTranslator(&translator);
    pump();
    editor.setLoading(true);
    require(hint->isHidden() && !enter(&editor, &files), "loading blocks file drops and feedback");
    editor.setLoading(false);
    require(enter(&editor, &files), "file dropping recovers after loading");
    editor.setExportBusy(true);
    require(hint->isHidden() && !enter(&editor, &files), "export blocks file drops and feedback");
    editor.setExportBusy(false);
    require(enter(&editor, &files), "file dropping recovers after export");
    editor.setLoading(true);
    QDropEvent busyDrop(editor.rect().center(), Qt::CopyAction, &files, Qt::LeftButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&editor, &busyDrop);
    require(!busyDrop.isAccepted() && drops == 1 && document.items().size() == 2,
            "a drop racing with busy work cannot import or swap images");
    editor.setLoading(false);
    require(enter(&editor, &files), "files can enter before hiding");
    editor.hide();
    require(hint->isHidden(), "hiding the editor clears stale drag feedback");
    editor.show();
    pump();
    QMimeData internal;
    internal.setData(QStringLiteral("application/x-snow-shot-collage-item"),
                     QByteArray::number(reinterpret_cast<quintptr>(&document)) + ':' +
                         QByteArray::number(id));
    QDragEnterEvent internalEnter(QRectF(editor.canvas()->rect()).center(), Qt::MoveAction,
                                  &internal, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor.canvas(), &internalEnter);
    require(internalEnter.isAccepted() && hint->isHidden(),
            "internal item drags retain swapping without an import hint");
    leave();
}

QImage renderCanvas(ImageStitchingPreviewCanvas& canvas, qreal dpr = 1) {
    QImage result(QSize(qCeil(canvas.width() * dpr), qCeil(canvas.height() * dpr)),
                  QImage::Format_ARGB32_Premultiplied);
    result.setDevicePixelRatio(dpr);
    result.fill(Qt::transparent);
    QPainter painter(&result);
    canvas.render(&painter, {}, {}, QWidget::DrawWindowBackground);
    return result;
}

void selectionOutlineMatchesPdfHandles() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalConfig = themes.config();
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        ImageStitchingDocument document;
        const auto id = document.append(image({120, 80}), QStringLiteral("selected"));
        ImageStitchingPreviewCanvas canvas(&document);
        canvas.resize(500, 400);
        canvas.show();
        pump();
        for (const int zoomDelta : {0, -120, 240}) {
            wheel(&canvas, {250, 200}, zoomDelta);
            mouse(&canvas, QEvent::MouseButtonPress, {20, 20}, Qt::MiddleButton, Qt::MiddleButton);
            mouse(&canvas, QEvent::MouseMove, {20.25, 20.5}, Qt::NoButton, Qt::MiddleButton);
            mouse(&canvas, QEvent::MouseButtonRelease, {20.25, 20.5}, Qt::MiddleButton,
                  Qt::NoButton);
            for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
                document.select(0);
                const auto plain = renderCanvas(canvas, dpr);
                auto expected = plain.copy();
                {
                    QPainter painter(&expected);
                    snowCanvasPaintImageSelection(painter, canvas.imageViewportRect(id));
                }
                document.select(id);
                require(renderCanvas(canvas, dpr) == expected,
                        "static selection must match PDF frame padding, strokes and white handles "
                        "at every DPI and zoom");
                document.select(0);
                require(renderCanvas(canvas, dpr) == plain,
                        "deselection removes every selection decoration");
            }
        }
    }
    themes.setConfig(originalConfig);
}

void replacementHighlightMatchesDrawEngineMarquee() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalConfig = themes.config();
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        ImageStitchingDocument document;
        const auto first = document.append(image({120, 80}), QStringLiteral("first"));
        const auto second = document.append(image({120, 80}, Qt::green), QStringLiteral("second"));
        auto settings = document.settings();
        settings.border = 20;
        settings.spacing = 20;
        document.setSettings(settings);
        ImageStitchingPreviewCanvas canvas(&document);
        canvas.resize(500, 400);
        canvas.show();
        pump();
        for (const int zoomDelta : {0, -120, 240}) {
            wheel(&canvas, {250, 200}, zoomDelta);
            const auto source = canvas.imageViewportRect(first).center();
            const auto target = canvas.cellViewportRect(second);
            for (const qreal dpr : {1.0, 1.25, 1.5, 2.0}) {
                document.select(0);
                auto expected = renderCanvas(canvas, dpr);
                mouse(&canvas, QEvent::MouseButtonPress, source, Qt::LeftButton, Qt::LeftButton);
                mouse(&canvas, QEvent::MouseMove, target.center(), Qt::NoButton, Qt::LeftButton);
                {
                    QPainter painter(&expected);
                    snowCanvasPaintSelectionMarquee(painter, target);
                    snowCanvasPaintImageSelection(painter, canvas.imageViewportRect(first));
                }
                const auto actual = renderCanvas(canvas, dpr);
                const QRect patch =
                    QRectF(target.topLeft() * dpr, target.size() * dpr).toAlignedRect();
                require(actual.copy(patch) == expected.copy(patch),
                        "swap targets must use PDF's solid one-pixel marquee and translucent fill");
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
                QApplication::sendEvent(&canvas, &escape);
                mouse(&canvas, QEvent::MouseButtonRelease, target.center(), Qt::LeftButton,
                      Qt::NoButton);
                require(document.items()[0].id == first &&
                            document.items()[0].staticPlacement == ImageStitchingStaticPlacement{},
                        "cancelling a replacement preview preserves ordering and placement");
            }
        }
    }
    themes.setConfig(originalConfig);
}

void staticPlacementOverflowAndDetailAlpha() {
    ImageStitchingDocument document;
    const auto id =
        document.append(image({1200, 800}, QColor(255, 0, 0, 128)), QStringLiteral("alpha"));
    auto settings = document.settings();
    settings.border = 160;
    document.setSettings(settings);
    ImageStitchingPreviewCanvas canvas(&document);
    canvas.resize(900, 760);
    canvas.show();
    pump();
    document.setStaticPlacement(id, {{}, 1.3});
    const auto cell = canvas.cellViewportRect(id);
    const QPoint inside = cell.center().toPoint();
    const QPoint outside = QPointF(cell.left() - 12, cell.center().y()).toPoint();
    const auto verifyAlpha = [&] {
        const auto result = renderCanvas(canvas);
        const auto full = result.pixelColor(inside);
        const auto ghost = result.pixelColor(outside);
        require(full.red() == 255 && std::abs(full.green() - 127) <= 1,
                "selected image alpha is composited once inside its slot");
        require(ghost.red() == 255 && std::abs(ghost.green() - 223) <= 2,
                "selected overflow uses PDF's quarter-opacity preview outside the slot");
    };
    verifyAlpha();
    QElapsedTimer elapsed;
    elapsed.start();
    while (canvas.property("detailTileCount").toInt() == 0 && elapsed.elapsed() < 4000) {
        pump();
        QThread::msleep(1);
    }
    require(canvas.property("detailTileCount").toInt() > 0,
            "edited large images prepare bounded detail tiles");
    verifyAlpha();
    const auto* toolbar = canvas.findChild<QWidget*>(QStringLiteral("collageImageToolbar"));
    require(toolbar && toolbar->isVisible(), "edited images retain their floating controls");
    for (const auto& corner : snowCanvasImageSelectionCorners(canvas.imageViewportRect(id)))
        require(
            !QRectF(toolbar->geometry()).intersects(QRectF(corner - QPointF(6, 6), QSizeF(12, 12))),
            "floating controls leave selected corner hit areas accessible");
    const auto fixturePath = qEnvironmentVariable("SNOW_TEST_STATIC_EDIT_PREVIEW");
    if (!fixturePath.isEmpty())
        require(canvas.grab().save(fixturePath), "save static editing visual fixture");
    document.select(0);
    require(renderCanvas(canvas).pixelColor(outside) == Qt::white,
            "unselected overflow is clipped completely");
}

void centeredStaticResizeMoveAndCancellation() {
    ImageStitchingDocument document;
    const auto first = document.append(image({240, 160}), QStringLiteral("first"));
    const auto second = document.append(image({240, 160}, Qt::blue), QStringLiteral("second"));
    auto settings = document.settings();
    settings.spacing = 20;
    document.setSettings(settings);
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1100, 800);
    editor.show();
    pump();
    auto* canvas = editor.canvas();
    const auto fitted = canvas->imageViewportRect(first);
    const auto slot = canvas->cellViewportRect(first);
    const auto originalSize = document.layout().size;
    const auto revision = document.contentRevision();
    int commits = 0;
    QObject::connect(&document, &ImageStitchingDocument::staticPlacementChanged, &editor,
                     [&](quint64) { ++commits; });
    const auto press = [&](QPointF point) {
        mouse(canvas, QEvent::MouseButtonPress, point, Qt::LeftButton, Qt::LeftButton);
    };
    const auto move = [&](QPointF point) {
        mouse(canvas, QEvent::MouseMove, point, Qt::NoButton, Qt::LeftButton);
    };
    const auto release = [&](QPointF point) {
        mouse(canvas, QEvent::MouseButtonRelease, point, Qt::LeftButton, Qt::NoButton);
    };
    const auto escape = [&] {
        QKeyEvent event(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(canvas, &event);
    };
    const auto frame = snowCanvasImageSelectionFrame(fitted);
    const std::array<QPointF, 8> handles{frame.topLeft(),
                                         frame.topRight(),
                                         frame.bottomRight(),
                                         frame.bottomLeft(),
                                         QPointF(frame.center().x(), frame.top()),
                                         QPointF(frame.right(), frame.center().y()),
                                         QPointF(frame.center().x(), frame.bottom()),
                                         QPointF(frame.left(), frame.center().y())};
    const std::array<Qt::CursorShape, 8> cursors{
        Qt::SizeFDiagCursor, Qt::SizeBDiagCursor, Qt::SizeFDiagCursor, Qt::SizeBDiagCursor,
        Qt::SizeVerCursor,   Qt::SizeHorCursor,   Qt::SizeVerCursor,   Qt::SizeHorCursor};
    for (std::size_t index = 0; index < handles.size(); ++index) {
        document.select(first);
        mouse(canvas, QEvent::MouseMove, handles[index], Qt::NoButton, Qt::NoButton);
        require(canvas->cursor().shape() == cursors[index],
                "every handle exposes its directional resize cursor");
        auto vector = handles[index] - frame.center();
        // Selection padding is not part of the image scaling vector.
        vector.setX(vector.x() == 0 ? 0 : std::copysign(fitted.width() / 2, vector.x()));
        vector.setY(vector.y() == 0 ? 0 : std::copysign(fitted.height() / 2, vector.y()));
        const auto finish = handles[index] - vector * .5;
        const int before = commits;
        press(handles[index]);
        move(handles[index] - vector * .25);
        require(commits == before &&
                    document.items()[0].staticPlacement == ImageStitchingStaticPlacement{},
                "resize preview is transient until release");
        release(finish);
        const auto smaller = canvas->imageViewportRect(first);
        require(std::abs(smaller.width() - fitted.width() / 2) < .01 &&
                    std::abs(smaller.height() - fitted.height() / 2) < .01 &&
                    (smaller.center() - fitted.center()).manhattanLength() < .01 &&
                    commits == before + 1 && document.contentRevision() == revision &&
                    document.layout().size == originalSize &&
                    canvas->cellViewportRect(first) == slot,
                "all handles resize proportionally about center using final release position and "
                "fixed slots");
        document.resetTransform(first);
    }
    const auto start = fitted.center();
    press(start);
    move(start + QPointF(20, 10));
    release(start + QPointF(30, 15));
    const auto moved = canvas->imageViewportRect(first);
    require((moved.center() - start - QPointF(30, 15)).manhattanLength() <= canvas->zoom(),
            "body dragging commits translation within the slot");
    const auto placement = document.items()[0].staticPlacement;
    for (int cancel = 0; cancel < 8; ++cancel) {
        document.select(first);
        const auto bounds = canvas->imageViewportRect(first);
        const auto handle = snowCanvasImageSelectionCorners(bounds)[2];
        press(handle);
        move(handle + QPointF(30, 20));
        switch (cancel) {
        case 0:
            escape();
            break;
        case 1:
            editor.setExportBusy(true);
            break;
        case 2:
            editor.setLoading(true);
            break;
        case 3:
            document.select(second);
            break;
        case 4:
            canvas->hide();
            break;
        case 5:
            wheel(canvas, canvas->rect().center(), 120);
            break;
        case 7:
            canvas->resize(canvas->width() + 20, canvas->height());
            break;
        case 6: {
            auto colors = document.settings();
            colors.background = Qt::yellow;
            document.setSettings(colors);
            break;
        }
        }
        release(handle + QPointF(30, 20));
        require(document.items()[0].staticPlacement == placement,
                "lifecycle changes cancel transient placement");
        if (cancel == 0)
            require(document.selectedId() == first, "Escape during editing retains selection");
        editor.setExportBusy(false);
        editor.setLoading(false);
        canvas->show();
        pump();
    }
    document.resetTransform(first);
    document.select(first);
    const auto handle = snowCanvasImageSelectionCorners(canvas->imageViewportRect(first))[2];
    const auto target = canvas->cellViewportRect(second).center();
    press(handle);
    move(target);
    require(canvas->property("swapTargetId").toULongLong() == 0,
            "resizing across another slot cannot swap");
    escape();
    release(target);
    const auto source = canvas->imageViewportRect(first).center();
    press(source);
    move(source + QPointF(12, 12));
    release(target);
    require(document.items()[0].id == second && document.selectedId() == first,
            "body dragging swaps by the release slot, not the previous pointer position");
}

void thumbnailScrollBarOverlaysTheList() {
    ImageStitchingDocument document;
    for (int i = 0; i < 10; ++i)
        document.append(image({32, 24}), QString::number(i));
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 700);
    editor.show();
    pump();
    auto* strip =
        editor.findChild<adqt::widgets::AdScrollArea*>(QStringLiteral("collageThumbnailStrip"));
    require(strip != nullptr, "thumbnail list must expose its scroll area");
    auto* overlay = strip->overlayHorizontalScrollBar();
    auto* source = strip->horizontalScrollBar();
    auto* content = strip->contentWidget();
    require(overlay && source && content, "thumbnail scrolling controls must exist");
    const auto verify = [&](bool overflow) {
        require((source->maximum() > 0) == overflow && overlay->isVisible() == overflow,
                "thumbnail overlay must appear only when the list overflows");
        require(!source->isVisible() && !strip->verticalScrollBar()->isVisible() &&
                    !strip->overlayVerticalScrollBar()->isVisible(),
                "thumbnail list must never show a second or vertical scrollbar");
        int visibleBars = 0;
        for (auto* bar : strip->findChildren<QScrollBar*>())
            visibleBars += bar->isVisible() ? 1 : 0;
        require(visibleBars == (overflow ? 1 : 0),
                "thumbnail list must have exactly one visible scrollbar when overflowing");
        require(strip->viewport()->geometry() == strip->rect() &&
                    strip->height() == content->height(),
                "thumbnail viewport must use the full strip without a dedicated scrollbar row");
        if (overflow) {
            const QRect contentBounds(content->pos(), content->size());
            require(overlay->parentWidget() == strip->viewport() &&
                        strip->viewport()->rect().contains(overlay->geometry()) &&
                        contentBounds.contains(overlay->geometry()) &&
                        overlay->maximum() == source->maximum() &&
                        overlay->pageStep() == strip->viewport()->width(),
                    "thumbnail scrollbar must float over the list and track its scroll range");
        }
    };
    verify(true);

    const QPointF trackEnd(overlay->width() - 4, overlay->height() - 2);
    mouse(overlay, QEvent::MouseButtonPress, trackEnd, Qt::LeftButton, Qt::LeftButton);
    mouse(overlay, QEvent::MouseButtonRelease, trackEnd, Qt::LeftButton, Qt::NoButton);
    require(source->value() > 0 && content->pos().x() == -source->value(),
            "clicking the floating scrollbar must scroll the thumbnails");
    source->setValue(0);
    const QPointF center = QRectF(strip->viewport()->rect()).center();
    QWheelEvent scroll(center, strip->viewport()->mapToGlobal(center.toPoint()), {},
                       QPoint(-120, 0), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(strip->viewport(), &scroll);
    require(source->value() > 0 && overlay->value() == source->value(),
            "horizontal wheel scrolling must work with the native scrollbar hidden");

    auto* first = editor.findChild<QWidget*>(
        QStringLiteral("collageThumbnail_%1").arg(document.items().first().id));
    require(first != nullptr, "first thumbnail must exist for wheel input");
    first->setFocus();
    pump();
    auto* replace = button(first, "collageReplaceImage");
    require(replace->isVisible(), "focused thumbnail actions must receive wheel input");
    const auto selected = document.selectedId();
    const auto zoom = editor.canvas()->zoom();
    for (auto* target : {strip->viewport(), content, first, static_cast<QWidget*>(replace),
                         static_cast<QWidget*>(overlay)}) {
        source->setValue(0);
        const QPointF point(4, 4);
        const auto shiftWheel = [&](int delta) {
            QWheelEvent event(point, target->mapToGlobal(point.toPoint()), {}, QPoint(0, delta),
                              Qt::NoButton, Qt::ShiftModifier, Qt::NoScrollPhase, false);
            // Use Qt's native input propagation through thumbnail children to the viewport.
            QSpontaneKeyEvent::setSpontaneous(&event);
            qApp->notify(target, &event);
            require(event.isAccepted(), "thumbnail list must accept Shift + wheel scrolling");
        };
        shiftWheel(-120);
        require(source->value() > 0 && overlay->value() == source->value() &&
                    content->x() == -source->value() && strip->verticalScrollBar()->value() == 0,
                "Shift + wheel over the list, thumbnails, actions, and overlay must scroll right");
        shiftWheel(120);
        require(source->value() == 0 && overlay->value() == 0 && content->x() == 0,
                "reversing Shift + wheel must scroll the thumbnails back left");
    }
    require(document.selectedId() == selected && editor.canvas()->zoom() == zoom,
            "scrolling thumbnails must preserve selection and canvas zoom");
    overlay->setValue(overlay->maximum());
    auto* last = editor.findChild<QWidget*>(
        QStringLiteral("collageThumbnail_%1").arg(document.items().last().id));
    require(last && strip->viewport()->rect().contains(
                        QRect(last->mapTo(strip->viewport(), QPoint{}), last->size())),
            "the floating scrollbar must reach the final thumbnail");

    editor.resize(2400, 700);
    pump();
    verify(false);
    editor.resize(1000, 700);
    pump();
    verify(true);
    while (document.items().size() > 1)
        document.remove(document.items().last().id);
    pump();
    verify(false);
    for (int i = 1; i < 10; ++i)
        document.append(image({32, 24}), QString::number(i));
    pump();
    verify(true);
    document.clear();
    pump();
    require(source->maximum() == 0 && !overlay->isVisible() && !source->isVisible(),
            "clearing the thumbnail list must hide its scrollbar");
    document.append(image({32, 24}), QStringLiteral("after clear"));
    pump();
    verify(false);
}
void thumbnailActionsUseTranslucentOverlays() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalConfig = themes.config();
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        ImageStitchingDocument document;
        const auto first = document.append(image({108, 84}), QStringLiteral("first"));
        const auto second = document.append(image({108, 84}, Qt::blue), QStringLiteral("second"));
        ImageStitchingEditorWidget editor(&document);
        editor.resize(1000, 700);
        editor.show();
        pump();
        quint64 replacement = 0;
        QObject::connect(&editor, &ImageStitchingEditorWidget::replaceImageRequested, &editor,
                         [&](quint64 id) { replacement = id; });
        for (const auto id : {first, second}) {
            auto* thumbnail =
                editor.findChild<QWidget*>(QStringLiteral("collageThumbnail_%1").arg(id));
            require(thumbnail != nullptr, "each image must have a thumbnail");
            thumbnail->setFocus();
            pump();
            auto* replace = button(thumbnail, "collageReplaceImage");
            auto* remove = button(thumbnail, "collageDeleteImage");
            for (auto* action : {replace, remove}) {
                require(dynamic_cast<OverlayControlButton*>(action) && action->isVisible() &&
                            thumbnail->rect().contains(action->geometry()) &&
                            action->focusPolicy() == Qt::StrongFocus &&
                            !action->accessibleName().isEmpty() &&
                            action->accessibleName() == action->toolTip(),
                        "thumbnail overlays must stay visible and accessible on keyboard focus");
                action->setAttribute(Qt::WA_UnderMouse, false);
                for (const qreal scale : {1.0, 1.5, 2.0}) {
                    QImage rendered(
                        QSize(qRound(action->width() * scale), qRound(action->height() * scale)),
                        QImage::Format_ARGB32_Premultiplied);
                    rendered.setDevicePixelRatio(scale);
                    rendered.fill(Qt::transparent);
                    {
                        QPainter painter(&rendered);
                        action->render(&painter, QPoint{}, QRegion{}, QWidget::DrawChildren);
                    }
                    const auto fill = rendered.pixelColor(qRound(4 * scale), rendered.height() / 2);
                    require(fill.alpha() > 0 && fill.alpha() < 255,
                            "thumbnail action backgrounds must let the image show through");
                    require(rendered.pixelColor(0, 0).alpha() == 0,
                            "thumbnail overlays must have transparent circular corners");
                }
            }
            require(!replace->geometry().intersects(remove->geometry()),
                    "thumbnail actions must have separate click targets");
            require(remove->x() - replace->geometry().right() - 1 == 4 &&
                        replace->y() == remove->y() &&
                        thumbnail->width() - remove->geometry().right() - 1 == 4 &&
                        thumbnail->height() - remove->geometry().bottom() - 1 == 4,
                    "thumbnail actions must have spacing and sit at the bottom-right corner");
            click(replace);
            require(replacement == id && document.items().size() == 2,
                    "the translucent replace action must target its own image");
            editor.setExportBusy(true);
            require(!replace->isEnabled() && !remove->isEnabled(),
                    "thumbnail actions must remain disabled during export");
            editor.setExportBusy(false);
        }
        click(button(editor.findChild<QWidget*>(QStringLiteral("collageThumbnail_%1").arg(second)),
                     "collageDeleteImage"));
        require(document.items().size() == 1 && document.items().first().id == first,
                "the translucent delete action must remove only its own image");
    }
    themes.setConfig(originalConfig);
}

void floatingBottomToolbarControlsCollection() {
    ImageStitchingDocument document;
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 650);
    editor.show();
    pump();
    auto* toolbar = editor.findChild<QWidget*>(QStringLiteral("collageCanvasToolbar"));
    auto* row = editor.findChild<QWidget*>(QStringLiteral("collageThumbnailRow"));
    auto* strip = editor.findChild<QWidget*>(QStringLiteral("collageThumbnailStrip"));
    auto* empty = editor.findChild<QWidget*>(QStringLiteral("collageEmptyState"));
    auto* add = button(&editor, "collageAddImages");
    auto* clear = button(&editor, "collageClearAll");
    auto* reverse = button(&editor, "collageReverseImages");
    require(
        toolbar && row && strip && empty && toolbar->isVisible() &&
            add->parentWidget() == toolbar && clear->parentWidget() == toolbar &&
            reverse->parentWidget() == toolbar && !row->isAncestorOf(add) &&
            !row->isAncestorOf(clear) && row->layout()->count() == 1 &&
            row->layout()->itemAt(0)->widget() == strip,
        "all collection actions float over the preview and leave the thumbnail row to its strip");
    const auto anchoredTo = [&](QWidget* preview) {
        const QRect previewRect(preview->mapTo(&editor, QPoint{}), preview->size());
        const QRect toolbarRect(toolbar->mapTo(&editor, QPoint{}), toolbar->size());
        require(previewRect.contains(toolbarRect) &&
                    std::abs(toolbarRect.center().x() - previewRect.center().x()) <= 1 &&
                    previewRect.bottom() - toolbarRect.bottom() == 12,
                "collection toolbar stays centered twelve pixels above the preview bottom");
    };
    anchoredTo(empty);
    require(add->isEnabled() && !add->busy() && !clear->isEnabled() && !reverse->isEnabled(),
            "empty collection enables addition while clearing and reversing stay disabled");
    for (auto* action : {add, clear, reverse})
        require(
            action->text().isEmpty() && !action->toolTip().isEmpty() &&
                action->accessibleName() == action->toolTip(),
            "floating collection icons expose translated names to keyboard and assistive input");
    int additions = 0;
    int clears = 0;
    QObject::connect(&editor, &ImageStitchingEditorWidget::addImagesRequested, &editor,
                     [&] { ++additions; });
    QObject::connect(&editor, &ImageStitchingEditorWidget::clearRequested, &editor,
                     [&] { ++clears; });
    add->click();
    require(additions == 1, "the floating add action requests import");
    QVector<quint64> ids;
    for (int index = 0; index < 12; ++index)
        ids.append(document.append(image({120, 80}), QStringLiteral("Image %1").arg(index + 1)));
    document.select(ids[4]);
    pump();
    require(editor.canvas()->isVisible() && strip->isVisible() && add->isEnabled() &&
                clear->isEnabled() && reverse->isEnabled(),
            "populating the collection enables clear and reverse without adding thumbnail-side "
            "buttons");
    anchoredTo(editor.canvas());
    reverse->click();
    for (qsizetype index = 0; index < ids.size(); ++index)
        require(document.items().at(index).id == ids.at(ids.size() - index - 1),
                "reverse changes the entire input order, including images outside the thumbnail "
                "viewport");
    require(document.selectedId() == ids[4],
            "reversing keeps selection attached to the same image");
    clear->click();
    require(clears == 1 && document.items().size() == ids.size(),
            "floating clear requests confirmation before changing the collection");
    editor.resize(1150, 760);
    pump();
    anchoredTo(editor.canvas());
    editor.setLoading(true);
    require(!add->isEnabled() && add->busy() && !reverse->isEnabled() && clear->isEnabled(),
            "loading marks addition busy, blocks reversal and keeps clearing available");
    reverse->click();
    require(document.items().first().id == ids.last(),
            "loading cannot reverse pending input order");
    editor.setLoading(false);
    editor.setExportBusy(true);
    require(toolbar->isVisible() && !add->isEnabled() && !add->busy() && !reverse->isEnabled() &&
                !clear->isEnabled(),
            "export preserves the floating toolbar while disabling collection mutations");
    editor.setExportBusy(false);
    require(add->isEnabled() && clear->isEnabled() && reverse->isEnabled(),
            "collection actions recover after export");
    document.setOutputType(ImageStitchingOutputType::PdfDocument);
    require(!toolbar->isVisible(), "PDF previews use their own floating collection toolbar");
    document.setOutputType(ImageStitchingOutputType::StaticImage);
    require(toolbar->isVisible(), "returning to static output restores its floating toolbar");
    document.clear();
    pump();
    require(add->isEnabled() && !clear->isEnabled() && !reverse->isEnabled(),
            "clearing restores the initial collection action states");
    anchoredTo(empty);
}
void floatingFitToCanvasFollowsViewport() {
    ImageStitchingDocument document;
    ImageStitchingEditorWidget editor(&document);
    auto* canvas = editor.canvas();
    auto* fit = button(&editor, "collageFitToCanvas");
    require(!fit->isEnabled(),
            "the empty preview must initialize its fit action before the editor is shown");
    editor.resize(1000, 700);
    editor.show();
    pump();
    require(dynamic_cast<OverlayControlButton*>(fit) && fit->parentWidget() == canvas &&
                fit->text().isEmpty() && fit->sizeClass() == AdButton::SizeClass::Medium &&
                fit->size() == QSize(32, 32) && fit->iconSize() == QSize(18, 18) &&
                fit->toolTip() == QStringLiteral("Fit to canvas") &&
                fit->accessibleName() == fit->toolTip(),
            "fit must reuse the accessible floating icon button inside the canvas");
    const auto verifyCorner = [&] {
        require(fit->isVisible() && canvas->rect().contains(fit->geometry()) &&
                    canvas->width() - fit->geometry().right() - 1 == 12 &&
                    canvas->height() - fit->geometry().bottom() - 1 == 12,
                "fit must stay at the canvas bottom-right corner");
        require(!fit->isEnabled() || canvas->childAt(fit->geometry().center()) == fit,
                "the enabled floating fit button must remain above the preview");
    };
    require(!fit->isVisible() && !fit->isEnabled(),
            "fit must stay hidden and disabled while the empty state replaces the preview");
    const auto first = document.append(image({800, 400}), QStringLiteral("first"));
    const auto second = document.append(image({400, 800}, Qt::blue), QStringLiteral("second"));
    pump();
    require(fit->isEnabled(), "adding images must enable the floating fit action");
    verifyCorner();
    const auto fittedZoom = canvas->zoom();
    const auto fittedOffset = canvas->offset();
    wheel(canvas, {120, 100}, 240);
    mouse(canvas, QEvent::MouseButtonPress, {50, 50}, Qt::MiddleButton, Qt::MiddleButton);
    mouse(canvas, QEvent::MouseMove, {140, 125}, Qt::NoButton, Qt::MiddleButton);
    mouse(canvas, QEvent::MouseButtonRelease, {140, 125}, Qt::MiddleButton, Qt::NoButton);
    verifyCorner();
    const auto selection = document.selectedId();
    click(fit);
    require(qFuzzyCompare(canvas->zoom(), fittedZoom) &&
                (canvas->offset() - fittedOffset).manhattanLength() < .001 &&
                document.selectedId() == selection &&
                QRectF(canvas->rect()).contains(canvas->imageViewportRect(first)) &&
                QRectF(canvas->rect()).contains(canvas->imageViewportRect(second)),
            "clicking fit must restore the full preview without changing selection");
    editor.resize(850, 550);
    pump();
    verifyCorner();
    editor.resize(1300, 800);
    pump();
    verifyCorner();
    const auto resizedZoom = canvas->zoom();
    wheel(canvas, {120, 100}, 120);
    fit->setFocus();
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(fit, &press);
    QApplication::sendEvent(fit, &release);
    require(qFuzzyCompare(canvas->zoom(), resizedZoom),
            "the floating fit action must remain keyboard accessible");
    document.clear();
    pump();
    require(!fit->isVisible() && !fit->isEnabled(),
            "clearing the result must hide and disable the floating fit action");
}
void arrangementChangesFitToCanvas() {
    ImageStitchingDocument document;
    const auto first = document.append(image({800, 400}), QStringLiteral("first"));
    const auto second = document.append(image({400, 800}, Qt::blue), QStringLiteral("second"));
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 700);
    editor.show();
    pump();
    auto* canvas = editor.canvas();
    auto* direction =
        editor.findChild<adqt::widgets::AdSegmented*>(QStringLiteral("collageDirection"));
    require(direction != nullptr, "arrangement control must be available");
    document.select(second);
    QStringList fields;
    QObject::connect(&editor, &ImageStitchingEditorWidget::settingsEdited, &editor,
                     [&](const QString& field) { fields.append(field); });
    for (const auto arrangement :
         {ImageStitchingDirection::Horizontal, ImageStitchingDirection::Vertical}) {
        wheel(canvas, {120, 100}, 480);
        mouse(canvas, QEvent::MouseButtonPress, {50, 50}, Qt::MiddleButton, Qt::MiddleButton);
        mouse(canvas, QEvent::MouseMove, {140, 125}, Qt::NoButton, Qt::MiddleButton);
        mouse(canvas, QEvent::MouseButtonRelease, {140, 125}, Qt::MiddleButton, Qt::NoButton);
        if (arrangement == ImageStitchingDirection::Horizontal) {
            auto* option = direction->childAt(direction->optionRect(1).center());
            require(option != nullptr, "horizontal arrangement must expose its button");
            click(option);
        } else {
            auto* option = direction->childAt(direction->optionRect(1).center());
            require(option != nullptr, "selected arrangement must expose its button");
            option->setFocus(Qt::TabFocusReason);
            QKeyEvent previous(QEvent::KeyPress, Qt::Key_Left, Qt::NoModifier);
            QApplication::sendEvent(option, &previous);
            pump();
        }
        require(document.settings().direction == arrangement &&
                    fields.size() == (arrangement == ImageStitchingDirection::Horizontal ? 1 : 2) &&
                    fields.last() == QStringLiteral("direction"),
                "mouse and keyboard arrangement changes must update and publish the direction");
        const auto fittedZoom = canvas->zoom();
        const auto fittedOffset = canvas->offset();
        require(document.selectedId() == second &&
                    QRectF(canvas->rect()).contains(canvas->imageViewportRect(first)) &&
                    QRectF(canvas->rect()).contains(canvas->imageViewportRect(second)),
                "switching either arrangement must fit every image and retain selection");
        canvas->fitToCanvas();
        require(qFuzzyCompare(canvas->zoom(), fittedZoom) &&
                    (canvas->offset() - fittedOffset).manhattanLength() < .001,
                "automatic arrangement fitting must match the Fit to canvas action");
    }
    wheel(canvas, {120, 100}, 120);
    const auto zoom = canvas->zoom();
    const auto anchor = canvas->imageViewportRect(second).center();
    auto* border =
        editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageOuterBorder"));
    require(border != nullptr, "border control must be available");
    border->setValue(17);
    require(document.settings().border == 17 && qFuzzyCompare(canvas->zoom(), zoom) &&
                (canvas->imageViewportRect(second).center() - anchor).manhattanLength() < .001,
            "other layout settings must preserve the zoom and selected image anchor");
}
void fitPanZoomAndNavigation() {
    ImageStitchingDocument document;
    const auto first = document.append(image({400, 200}), QStringLiteral("first"));
    const auto second = document.append(image({100, 400}, Qt::blue), QStringLiteral("second"));
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 700);
    editor.show();
    pump();
    auto* canvas = editor.canvas();
    auto* toolbar = canvas->findChild<QWidget*>(QStringLiteral("collageImageToolbar"));
    require(toolbar && toolbar->isVisible() &&
                QRectF(canvas->rect()).contains(QRectF(toolbar->geometry())) &&
                (std::abs(QRectF(toolbar->geometry()).center().x() -
                          canvas->imageViewportRect(first).center().x()) < 1.5 ||
                 !QRectF(toolbar->geometry()).intersects(canvas->imageViewportRect(first))),
            "initial selected image toolbar must be anchored before first show");
    auto* settings = editor.findChild<QWidget*>(QStringLiteral("collageSettingsScroll"));
    auto* add = button(&editor, "collageAddImages");
    require(settings &&
                settings->mapTo(&editor, QPoint{}).x() >
                    canvas->mapTo(&editor, QPoint{}).x() + canvas->width() &&
                add->mapTo(&editor, QPoint{}).x() + add->width() <=
                    canvas->mapTo(&editor, QPoint{}).x() + canvas->width(),
            "the preview and its image-management strip share the left column beside settings");
    canvas->fitToCanvas();
    require(QRectF(canvas->rect()).contains(canvas->imageViewportRect(first)),
            "first image must fit initially");
    require(QRectF(canvas->rect()).contains(canvas->imageViewportRect(second)),
            "complete collage must fit initially");
    const auto cursor = QPointF(canvas->width() * .7, canvas->height() * .4);
    const auto outputPoint = (cursor - canvas->offset()) / canvas->zoom();
    wheel(canvas, cursor, 120);
    const auto afterZoom = (cursor - canvas->offset()) / canvas->zoom();
    require((outputPoint - afterZoom).manhattanLength() < .001,
            "wheel zoom must keep pointer output position stable");
    const auto previous = canvas->offset();
    mouse(canvas, QEvent::MouseButtonPress, {50, 50}, Qt::MiddleButton, Qt::MiddleButton);
    mouse(canvas, QEvent::MouseMove, {80, 75}, Qt::NoButton, Qt::MiddleButton);
    mouse(canvas, QEvent::MouseButtonRelease, {80, 75}, Qt::MiddleButton, Qt::NoButton);
    require((canvas->offset() - previous - QPointF(30, 25)).manhattanLength() < .001,
            "middle-button dragging must pan the viewport");
    const auto previousZoom = canvas->zoom();
    auto* thumbnail = editor.findChild<QWidget*>(QStringLiteral("collageThumbnail_%1").arg(second));
    require(thumbnail != nullptr, "thumbnail must exist");
    click(thumbnail);
    require(document.selectedId() == second, "thumbnail must select its image");
    require(canvas->zoom() <= previousZoom, "thumbnail navigation must never zoom in");
    require((canvas->imageViewportRect(second).center() - QRectF(canvas->rect()).center())
                    .manhattanLength() < .001,
            "thumbnail navigation must center image");
    require(QRectF(canvas->rect()).contains(canvas->imageViewportRect(second)),
            "thumbnail navigation must show complete image");
    const auto anchor = canvas->imageViewportRect(second).center();
    click(button(canvas, "collageRotateClockwise"));
    require(transformedSize(document.items()[1]) == QSize(400, 100),
            "floating toolbar must rotate selected item");
    require((canvas->imageViewportRect(second).center() - anchor).manhattanLength() < .001,
            "editing must preserve selected image viewport anchor");
    click(button(canvas, "collageResetTransform"));
    require(document.items()[1].transform.isIdentity(),
            "reset toolbar action must reset selected transform");
}
void releaseOnlySwapAndCrossWindowIsolation() {
    ImageStitchingDocument document;
    const auto first = document.append(image({120, 80}), QStringLiteral("A"));
    const auto second = document.append(image({120, 80}), QStringLiteral("B"));
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 650);
    editor.show();
    pump();
    auto* canvas = editor.canvas();
    auto a = canvas->imageViewportRect(first).center();
    auto b = canvas->imageViewportRect(second).center();
    mouse(canvas, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
    require(document.items()[0].id == first, "drag must not swap before release");
    mouse(canvas, QEvent::MouseButtonRelease, {2, 2}, Qt::LeftButton, Qt::NoButton);
    require(document.items()[0].id == first, "release outside image must preserve order");
    document.resetTransform(first);
    mouse(canvas, QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton);
    mouse(canvas, QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton);
    require(document.items()[0].id == second && document.items()[1].id == first,
            "release over valid target must swap images");
    auto* thumbnail = editor.findChild<QWidget*>(QStringLiteral("collageThumbnail_%1").arg(first));
    require(thumbnail != nullptr && thumbnail->property("collageItemId").toULongLong() == first,
            "thumbnail identity must follow canvas swaps");
    ImageStitchingDocument other;
    other.append(image({100, 100}), QStringLiteral("other"));
    QMimeData foreign;
    foreign.setData(QStringLiteral("application/x-snow-shot-collage-item"),
                    QByteArray::number(reinterpret_cast<quintptr>(&other)) + ':' +
                        QByteArray::number(first));
    QDragEnterEvent enter(b, Qt::MoveAction, &foreign, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &enter);
    require(!enter.isAccepted(), "dragging from another collage must be rejected");
    QMimeData local;
    local.setData(QStringLiteral("application/x-snow-shot-collage-item"),
                  QByteArray::number(reinterpret_cast<quintptr>(&document)) + ':' +
                      QByteArray::number(second));
    auto* target = editor.findChild<QWidget*>(QStringLiteral("collageThumbnail_%1").arg(first));
    QDragEnterEvent localEnter(QPointF(20, 20), Qt::MoveAction, &local, Qt::LeftButton,
                               Qt::NoModifier);
    QApplication::sendEvent(target, &localEnter);
    require(localEnter.isAccepted(), "same collage thumbnail drag must be accepted");
    QDropEvent drop({20, 20}, Qt::MoveAction, &local, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &drop);
    require(drop.isAccepted() && document.items()[0].id == first,
            "thumbnail drop must swap document and preview order");
}
void invalidLayoutKeepsSelectionAndTransformsUsable() {
    ImageStitchingDocument document;
    const auto first = document.append(image({80, 40}), QStringLiteral("first"));
    const auto second = document.append(image({40, 80}, Qt::blue), QStringLiteral("second"));
    const auto firstPixels = document.items()[0].image.cacheKey();
    const auto secondPixels = document.items()[1].image.cacheKey();
    const auto initialSettings = document.settings();
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 650);
    editor.show();
    pump();
    auto* canvas = editor.canvas();
    auto* toolbar = canvas->findChild<QWidget*>(QStringLiteral("collageImageToolbar"));
    auto invalidSettings = initialSettings;
    invalidSettings.border = std::numeric_limits<int>::max();
    document.setSettings(invalidSettings);
    require(!document.layout().isValid() && canvas->imageViewportRect(first).isEmpty(),
            "resource-limited layout must retain images without invalid preview geometry");
    require(!button(canvas, "collageFitToCanvas")->isEnabled(),
            "fit must be disabled when the preview layout is invalid");
    pump();
    require(toolbar && toolbar->isVisible() && canvas->rect().contains(toolbar->geometry()) &&
                toolbar->geometry().top() > canvas->height() / 2,
            "invalid layout must retain transform controls below the centered error");
    canvas->centerImage(std::numeric_limits<quint64>::max());
    require(document.selectedId() == first, "invalid navigation IDs must preserve selection");
    auto* thumbnail = editor.findChild<QWidget*>(QStringLiteral("collageThumbnail_%1").arg(second));
    require(thumbnail != nullptr, "invalid layout must retain its thumbnails");
    click(thumbnail);
    require(document.selectedId() == second,
            "thumbnail selection must remain available while layout geometry is invalid");
    click(button(canvas, "collageRotateClockwise"));
    require(transformedSize(document.items()[1]) == QSize(80, 40),
            "invalid layout must allow rotating the selected image");
    click(button(canvas, "collageResetTransform"));
    require(document.items()[1].transform.isIdentity() &&
                document.items()[0].transform.isIdentity() && document.items().size() == 2 &&
                document.items()[0].id == first && document.items()[1].id == second &&
                document.settings() == invalidSettings &&
                document.items()[0].image.cacheKey() == firstPixels &&
                document.items()[1].image.cacheKey() == secondPixels,
            "recovery controls must preserve collection, settings, and original image pixels");
    editor.setExportBusy(true);
    require(!toolbar->isVisible(), "export must still hide transform recovery controls");
    editor.setExportBusy(false);
    require(toolbar->isVisible(), "recovery controls must return when export is no longer busy");
    document.setSettings(initialSettings);
    pump();
    require(document.layout().isValid() && button(canvas, "collageFitToCanvas")->isEnabled() &&
                !canvas->imageViewportRect(second).isEmpty() && document.selectedId() == second &&
                toolbar->isVisible(),
            "correcting resource-limited settings must restore the selected image preview");
}
void backgroundColorsShareRowAndCenteredSwatches() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalConfig = themes.config();
    for (const auto theme : {adqt::theme::ThemeScheme::Light, adqt::theme::ThemeScheme::Dark}) {
        themes.setPreset(theme);
        ImageStitchingDocument document;
        ImageStitchingEditorWidget editor(&document);
        editor.resize(1000, 650);
        editor.show();
        pump();
        auto* row = editor.findChild<QWidget*>(QStringLiteral("collageBackgroundColors"));
        auto* picker = editor.findChild<adqt::widgets::AdColorPicker*>(
            QStringLiteral("collageBackgroundColor"));
        auto* divider =
            editor.findChild<adqt::widgets::AdDivider*>(QStringLiteral("collageColorSeparator"));
        require(row && picker && divider && divider->vertical(),
                "background colors must have a vertical separator");
        require(!picker->triggerTextVisible() && picker->width() == picker->height(),
                "custom color picker must use a single square button");
        const auto bounds = [row](QWidget* widget) {
            return QRectF(widget->mapTo(row, QPoint{}), widget->size());
        };
        const auto pickerBounds = bounds(picker);
        const auto dividerBounds = bounds(divider);
        require(QRectF(row->rect()).contains(pickerBounds) &&
                    dividerBounds.right() < pickerBounds.left(),
                "custom color picker must fit after the separator");
        const auto separatorPixels = divider->grab().toImage();
        require(
            separatorPixels.pixelColor(separatorPixels.width() / 2, separatorPixels.height() / 2) !=
                separatorPixels.pixelColor(separatorPixels.width() - 1,
                                           separatorPixels.height() / 2),
            "separator must paint a visible vertical rail between color controls");
        constexpr std::array<const char*, 8> colors{"#FFFFFF", "#000000", "#8C8C8C", "#FFCCC7",
                                                    "#D9F7BE", "#BAE0FF", "#FFF1B8", "#EFDBFF"};
        qreal previousRight = -1;
        for (std::size_t i = 0; i < colors.size(); ++i) {
            auto* preset =
                editor.findChild<AdButton*>(QStringLiteral("collageColorPreset_%1").arg(i));
            require(preset != nullptr, "quick color preset must remain available");
            require(preset->shape() == AdButton::Shape::Rounded &&
                        preset->buttonStyle() == (preset->isChecked()
                                                      ? AdButton::ButtonStyle::Tonal
                                                      : AdButton::ButtonStyle::Text),
                    "presets must use the drawing toolbar's borderless and selected tonal styles");
            const auto presetBounds = bounds(preset);
            require(QRectF(row->rect()).contains(presetBounds) &&
                        presetBounds.left() > previousRight &&
                        presetBounds.right() < dividerBounds.left() &&
                        std::abs(presetBounds.center().y() - pickerBounds.center().y()) <= 1,
                    "quick colors must fit side by side and align with the custom picker");
            previousRight = presetBounds.right();
            const QColor color(QString::fromLatin1(colors[i]));
            click(preset);
            require(document.settings().background == color && preset->isChecked() &&
                        picker->value().solidColor == color &&
                        preset->buttonStyle() == AdButton::ButtonStyle::Tonal &&
                        preset->accentRole() == AdButton::AccentRole::Primary,
                    "clicking a quick color must update selection and custom picker");
            AdButton toolbarSelection(preset->parentWidget());
            toolbarSelection.setSizeClass(AdButton::SizeClass::Small);
            toolbarSelection.setShape(AdButton::Shape::Rounded);
            toolbarSelection.setButtonStyle(AdButton::ButtonStyle::Tonal);
            toolbarSelection.setAccentRole(AdButton::AccentRole::Primary);
            toolbarSelection.setFixedSize(preset->size());
            toolbarSelection.move(-100, -100);
            toolbarSelection.show();
            preset->setAttribute(Qt::WA_UnderMouse, false);
            toolbarSelection.setAttribute(Qt::WA_UnderMouse, false);
            const QPoint backgroundPixel(1, preset->height() / 2);
            require(preset->grab().toImage().pixelColor(backgroundPixel) ==
                        toolbarSelection.grab().toImage().pixelColor(backgroundPixel),
                    "selected quick colors must use the drawing toolbar's resting tonal blue");
            for (const qreal scale : {1.0, 1.5, 2.0}) {
                QImage rendered(
                    QSize(qRound(preset->width() * scale), qRound(preset->height() * scale)),
                    QImage::Format_ARGB32_Premultiplied);
                rendered.setDevicePixelRatio(scale);
                rendered.fill(Qt::transparent);
                {
                    QPainter painter(&rendered);
                    preset->render(&painter);
                }
                QRect swatch;
                const auto center = rendered.rect().center();
                for (int y = 0; y < rendered.height(); ++y) {
                    for (int x = 0; x < rendered.width(); ++x) {
                        if ((x == center.x() || y == center.y()) &&
                            rendered.pixelColor(x, y) == color)
                            swatch = swatch.united(QRect(x, y, 1, 1));
                    }
                }
                require(
                    swatch.width() == swatch.height() && swatch.width() >= qCeil(21 * scale) - 2 &&
                        swatch.width() <= qCeil(21 * scale) &&
                        std::abs(swatch.left() - (rendered.width() - swatch.right() - 1)) <= 1 &&
                        std::abs(swatch.top() - (rendered.height() - swatch.bottom() - 1)) <= 1,
                    "quick swatches must keep their solid fill centered inside the border");
                const int edge = qFloor(3 * scale);
                const QColor border = themes.resolveTheme(preset).colorBorder;
                for (const QPoint point :
                     {QPoint(edge, center.y()), QPoint(rendered.width() - edge - 1, center.y()),
                      QPoint(center.x(), edge), QPoint(center.x(), rendered.height() - edge - 1)}) {
                    const QColor pixel = rendered.pixelColor(point);
                    require(pixel != color,
                            "quick swatches must have a visible outline on each side");
                    if (scale > 1.0)
                        require(pixel == border,
                                "quick swatch borders must use the drawing toolbar's theme token");
                }
            }
            click(preset);
            require(preset->isChecked() && preset->buttonStyle() == AdButton::ButtonStyle::Tonal,
                    "clicking the current preset must preserve its selected appearance");
        }
        editor.setExportBusy(true);
        require(!picker->isEnabled() && !button(&editor, "collageColorPreset_1")->isEnabled(),
                "export must disable both custom and quick color edits");
    }
    themes.setConfig(originalConfig);
}

void controlsEmptyBusyAndPresets() {
    ImageStitchingDocument document;
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 650);
    editor.show();
    pump();
    require(!button(&editor, "collageClearAll")->isEnabled(), "clear must be disabled when empty");
    require(button(&editor, "collageEmptyAddImages")->isVisible(),
            "empty state must permit image addition");
    auto* empty = editor.findChild<QWidget*>(QStringLiteral("collageEmptyState"));
    auto* prompt = editor.findChild<QLabel*>(QStringLiteral("collageEmptyPrompt"));
    auto* strip = editor.findChild<QWidget*>(QStringLiteral("collageThumbnailStrip"));
    require(empty && prompt && strip, "empty state and thumbnail strip must exist");
    const auto requireEmpty = [&] {
        require(empty->isVisible() && prompt->isVisible() && !editor.canvas()->isVisible() &&
                    !strip->isVisible() && button(&editor, "collageAddImages")->isVisible() &&
                    button(&editor, "collageClearAll")->isVisible() &&
                    !button(&editor, "collageClearAll")->isEnabled(),
                "empty state replaces the canvas and thumbnails while keeping floating actions "
                "available");
        auto* guide = button(&editor, "collageEmptyAddImages");
        require(empty->isAncestorOf(guide) && empty->isAncestorOf(prompt) &&
                    prompt->geometry().bottom() < guide->geometry().top() &&
                    empty->rect().contains(guide->geometry()) && prompt->wordWrap(),
                "prompt and guide button must stay together in a wrapping layout");
    };
    requireEmpty();
    editor.setLoading(true);
    require(!button(&editor, "collageEmptyAddImages")->isEnabled() &&
                button(&editor, "collageEmptyAddImages")->busy(),
            "empty import must disable and busy the guide");
    requireEmpty();
    editor.setLoading(false);
    editor.setExportBusy(true);
    require(!button(&editor, "collageEmptyAddImages")->isEnabled(),
            "export must block duplicate imports from the guide");
    editor.setExportBusy(false);
    require(button(&editor, "collageEmptyAddImages")->isEnabled() &&
                !button(&editor, "collageEmptyAddImages")->busy(),
            "guide must recover after busy work finishes");
    int additions = 0;
    int clears = 0;
    quint64 replacement = 0;
    QStringList fields;
    QObject::connect(&editor, &ImageStitchingEditorWidget::addImagesRequested, &editor,
                     [&] { ++additions; });
    QObject::connect(&editor, &ImageStitchingEditorWidget::clearRequested, &editor,
                     [&] { ++clears; });
    QObject::connect(&editor, &ImageStitchingEditorWidget::replaceImageRequested, &editor,
                     [&](quint64 id) { replacement = id; });
    QObject::connect(&editor, &ImageStitchingEditorWidget::settingsEdited, &editor,
                     [&](const QString& field) { fields.append(field); });
    click(button(&editor, "collageEmptyAddImages"));
    require(additions == 1, "empty Add action must request import");
    auto* guide = button(&editor, "collageEmptyAddImages");
    guide->setFocus(Qt::TabFocusReason);
    require(guide->hasFocus(), "empty-state action must accept keyboard focus");
    QKeyEvent guidePress(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
    QKeyEvent guideRelease(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
    QApplication::sendEvent(guide, &guidePress);
    QApplication::sendEvent(guide, &guideRelease);
    require(additions == 2, "keyboard activation must request image import");
    const auto id = document.append(image({200, 100}), QStringLiteral("sample"));
    pump();
    require(!empty->isVisible() && editor.canvas()->isVisible() && strip->isVisible() &&
                button(&editor, "collageAddImages")->isVisible(),
            "adding an image must restore the preview and thumbnail controls");
    require(
        editor.canvas()->rect().contains(editor.canvas()->imageViewportRect(id).toAlignedRect()),
        "first image must fit the restored preview");
    auto* direction =
        editor.findChild<adqt::widgets::AdSegmented*>(QStringLiteral("collageDirection"));
    require(direction != nullptr, "layout control must use the standard segmented control");
    auto* horizontal = direction->childAt(direction->optionRect(1).center());
    require(horizontal != nullptr, "horizontal option must expose its interactive button");
    click(horizontal);
    require(document.settings().direction == ImageStitchingDirection::Horizontal &&
                fields.contains(QStringLiteral("direction")),
            "layout control must edit document and publish persistence field");
    auto* sizing = editor.findChild<adqt::widgets::AdSegmented*>(QStringLiteral("collageSizing"));
    require(sizing != nullptr, "image sizing must use the standard segmented control");
    auto* sizingOption = sizing->childAt(sizing->optionRect(sizing->currentIndex()).center());
    require(sizingOption != nullptr, "selected image sizing must expose its interactive button");
    sizingOption->setFocus(Qt::TabFocusReason);
    QKeyEvent nextSizing(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QApplication::sendEvent(sizingOption, &nextSizing);
    require(document.settings().sizing == ImageStitchingSizing::Original &&
                fields.contains(QStringLiteral("sizing_mode")),
            "keyboard selection must edit image sizing and publish persistence field");
    auto* border =
        editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageOuterBorder"));
    border->setValue(17);
    require(document.settings().border == 17 && fields.contains(QStringLiteral("border_px")),
            "outer border must use output pixels");
    constexpr std::array<const char*, 8> colors{"#FFFFFF", "#000000", "#8C8C8C", "#FFCCC7",
                                                "#D9F7BE", "#BAE0FF", "#FFF1B8", "#EFDBFF"};
    for (std::size_t i = 0; i < colors.size(); ++i) {
        auto* preset = editor.findChild<AdButton*>(QStringLiteral("collageColorPreset_%1").arg(i));
        preset->click();
        require(document.settings().background == QColor(QString::fromLatin1(colors[i])),
                "preset color must match fixed value");
        require(!preset->accessibleName().isEmpty() && !preset->toolTip().isEmpty(),
                "color presets must be accessible");
    }
    auto* picker =
        editor.findChild<adqt::widgets::AdColorPicker*>(QStringLiteral("collageBackgroundColor"));
    picker->setValue(adqt::widgets::AdColorValue::solid(QColor(10, 20, 30, 100)));
    require(document.settings().background.alpha() == 100,
            "semitransparent background must remain unchanged");
    picker->setValue(adqt::widgets::AdColorValue::none());
    require(document.settings().background.alpha() == 0,
            "clearing color must select transparent background");
    auto* thumbnail = editor.findChild<QWidget*>(QStringLiteral("collageThumbnail_%1").arg(id));
    thumbnail->setFocus();
    pump();
    auto* replace = button(thumbnail, "collageReplaceImage");
    require(replace->isVisible() && !replace->accessibleName().isEmpty(),
            "keyboard focus must reveal accessible thumbnail actions");
    replace->click();
    require(replacement == id, "replace action must identify stable item");
    button(&editor, "collageClearAll")->click();
    require(clears == 1 && document.items().size() == 1,
            "clear must request confirmation without modifying collection");
    editor.setLoading(true);
    require(!button(&editor, "collageAddImages")->isEnabled() && !replace->isEnabled() &&
                button(thumbnail, "collageDeleteImage")->isEnabled() &&
                button(&editor, "collageClearAll")->isEnabled() && border->isEnabled(),
            "loading prevents new imports and replacement while existing edits and clearing remain "
            "available");
    editor.setLoading(false);
    const auto second = document.append(image({200, 100}, Qt::blue), QStringLiteral("second"));
    editor.setExportBusy(true);
    require(!button(&editor, "collageAddImages")->isEnabled() && !border->isEnabled(),
            "export must prevent edits and duplicate imports");
    const auto zoom = editor.canvas()->zoom();
    wheel(editor.canvas(), {100, 100}, 120);
    require(editor.canvas()->zoom() > zoom, "busy export must leave viewport responsive");
    const auto secondCenter = editor.canvas()->imageViewportRect(second).center();
    mouse(editor.canvas(), QEvent::MouseButtonPress, secondCenter, Qt::LeftButton, Qt::LeftButton);
    mouse(editor.canvas(), QEvent::MouseButtonRelease, secondCenter, Qt::LeftButton, Qt::NoButton);
    require(document.selectedId() == second, "busy export must leave canvas selection available");
    click(thumbnail);
    require(document.selectedId() == id, "busy export must leave thumbnail navigation available");
    editor.setExportBusy(false);
    document.clear();
    pump();
    require(button(&editor, "collageEmptyAddImages")->isVisible(),
            "clear must restore usable empty state");
    requireEmpty();
    const auto last = document.append(image({120, 80}), QStringLiteral("last"));
    pump();
    document.remove(last);
    pump();
    requireEmpty();
    require(!editor.findChild<QWidget*>(QStringLiteral("collageStatus")) &&
                !editor.findChild<QWidget*>(QStringLiteral("collageStatusScroll")),
            "editor feedback has no bottom status panel that can displace the preview");
}
void spacingUnitsPreservePixelsAndConvertEdits() {
    ImageStitchingDocument document;
    document.append(image({300, 100}), QStringLiteral("first"));
    document.append(image({600, 200}), QStringLiteral("second"));
    auto settings = document.settings();
    settings.border = 17;
    settings.spacing = 11;
    document.setSettings(settings);
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 650);
    editor.show();
    pump();
    auto* border =
        editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageOuterBorder"));
    auto* spacing =
        editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageImageSpacing"));
    auto* borderUnit = editor.findChild<adqt::widgets::AdSegmented*>(
        QStringLiteral("collageOuterBorderUnitSegmented"));
    auto* spacingUnit = editor.findChild<adqt::widgets::AdSegmented*>(
        QStringLiteral("collageImageSpacingUnitSegmented"));
    auto* label = editor.findChild<QLabel*>(QStringLiteral("collageOuterBorderLabel"));
    require(border && spacing && borderUnit && spacingUnit && label,
            "both length fields must provide a label, numeric input and unit switch");
    require(borderUnit->controlSize() == adqt::widgets::AdSegmented::ControlSize::Small &&
                borderUnit->distribution() == adqt::widgets::AdSegmented::Distribution::Content &&
                label->geometry().right() < borderUnit->geometry().left() &&
                border->geometry().top() > borderUnit->geometry().bottom(),
            "length fields must match the save dialog's label and compact unit header");
    QStringList edits;
    QObject::connect(&editor, &ImageStitchingEditorWidget::settingsEdited, &editor,
                     [&](const QString& field) { edits.append(field); });
    border->setValue(17.4);
    spacing->setValue(10.6);
    require(border->value() == 17 && spacing->value() == 11 && border->decimals() == 0 &&
                spacing->decimals() == 0 && document.settings() == settings && edits.isEmpty(),
            "pixel inputs must round fractional values before applying layout settings");
    const auto layout = document.layout();
    const auto zoom = editor.canvas()->zoom();
    const auto offset = editor.canvas()->offset();
    for (int i = 0; i < 4; ++i) {
        borderUnit->setCurrentValue(QStringLiteral("percentage"));
        require(border->value() == 6 && border->decimals() == 0 &&
                    border->suffixText() == QStringLiteral("%") && spacing->decimals() == 0,
                "percentage display must use integers independently of the pixel spacing field");
        spacingUnit->setCurrentValue(QStringLiteral("percentage"));
        require(spacing->value() == 4 && spacing->decimals() == 0,
                "spacing percentages must also round to integers");
        borderUnit->setCurrentValue(QStringLiteral("pixels"));
        spacingUnit->setCurrentValue(QStringLiteral("pixels"));
    }
    require(document.settings() == settings && edits.isEmpty() && border->value() == 17 &&
                spacing->value() == 11 && document.layout().size == layout.size &&
                document.layout().rects == layout.rects && editor.canvas()->zoom() == zoom &&
                editor.canvas()->offset() == offset,
            "repeated unit switches must preserve exact output pixels, viewport and persistence");
    borderUnit->setCurrentValue(QStringLiteral("percentage"));
    spacingUnit->setCurrentValue(QStringLiteral("percentage"));
    border->setValue(12.5);
    spacing->setValue(2.5);
    require(border->value() == 13 && spacing->value() == 3 && document.settings().border == 39 &&
                document.settings().spacing == 9 &&
                edits == QStringList{QStringLiteral("border_px"), QStringLiteral("spacing_px")} &&
                document.layout().size == QSize(378, 287),
            "percentage edits must round to integers before updating preview and persistence");
    border->stepUp();
    spacing->stepDown();
    require(border->value() == 14 && spacing->value() == 2 && document.settings().border == 42 &&
                document.settings().spacing == 6,
            "percentage steps must change integer values by one");
    border->setValue(0);
    spacing->setValue(0);
    require(document.settings().border == 0 && document.settings().spacing == 0,
            "zero percentages must remove border and spacing");
    auto* line = border->findChild<QLineEdit*>();
    require(line != nullptr, "length input must expose its editor");
    border->focusEditor(adqt::widgets::AdInputNumber::FocusSelection::SelectAll);
    QKeyEvent decimalTyping(QEvent::KeyPress, Qt::Key_1, Qt::NoModifier, QStringLiteral("12.5"));
    QApplication::sendEvent(line, &decimalTyping);
    require(document.settings().border == 0, "typed percentages must wait for commit");
    borderUnit->setCurrentValue(QStringLiteral("pixels"));
    require(
        border->value() == 39 && document.settings().border == 39,
        "committed fractional text must round to an integer percentage before pixel conversion");
    border->setValue(0);
    border->focusEditor(adqt::widgets::AdInputNumber::FocusSelection::SelectAll);
    QKeyEvent typing(QEvent::KeyPress, Qt::Key_2, Qt::NoModifier, QStringLiteral("23"));
    QApplication::sendEvent(line, &typing);
    require(document.settings().border == 0, "typed pixels must wait for commit");
    borderUnit->setCurrentValue(QStringLiteral("percentage"));
    require(document.settings().border == 23 && border->value() == 8,
            "switching units must commit pending input in its previous unit");
    border->focusEditor(adqt::widgets::AdInputNumber::FocusSelection::SelectAll);
    QKeyEvent percentTyping(QEvent::KeyPress, Qt::Key_1, Qt::NoModifier, QStringLiteral("10"));
    QApplication::sendEvent(line, &percentTyping);
    borderUnit->setCurrentValue(QStringLiteral("pixels"));
    require(document.settings().border == 30 && border->value() == 30,
            "pending percentages must commit before switching back to pixels");
    spacing->setValue(125);
    require(document.settings().spacing == 375,
            "percentage spacing must support values above one hundred percent");
    editor.setExportBusy(true);
    require(!borderUnit->isEnabled() && !spacingUnit->isEnabled(),
            "export must disable both unit switches with the settings form");
}

void spacingPercentagesFollowImageReference() {
    ImageStitchingDocument document;
    const auto first = document.append(image({200, 80}), QStringLiteral("first"));
    document.append(image({400, 100}), QStringLiteral("second"));
    auto settings = document.settings();
    settings.border = 20;
    settings.spacing = 10;
    document.setSettings(settings);
    ImageStitchingEditorWidget editor(&document);
    auto* border =
        editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageOuterBorder"));
    auto* spacing =
        editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageImageSpacing"));
    auto* borderUnit = editor.findChild<adqt::widgets::AdSegmented*>(
        QStringLiteral("collageOuterBorderUnitSegmented"));
    auto* spacingUnit = editor.findChild<adqt::widgets::AdSegmented*>(
        QStringLiteral("collageImageSpacingUnitSegmented"));
    borderUnit->setCurrentValue(QStringLiteral("percentage"));
    spacingUnit->setCurrentValue(QStringLiteral("percentage"));
    require(border->value() == 10 && spacing->value() == 5,
            "uniform vertical percentages must use the first image width");
    settings.sizing = ImageStitchingSizing::Original;
    document.setSettings(settings);
    require(border->value() == 5 && spacing->value() == 3,
            "original vertical percentages must use the widest image");
    settings.direction = ImageStitchingDirection::Horizontal;
    document.setSettings(settings);
    require(border->value() == 20 && spacing->value() == 10,
            "original horizontal percentages must use the tallest image");
    settings.sizing = ImageStitchingSizing::Uniform;
    document.setSettings(settings);
    require(border->value() == 25 && spacing->value() == 13,
            "uniform horizontal percentages must use the first image height");
    document.rotate(first, true);
    require(border->value() == 10 && spacing->value() == 5 && document.settings() == settings,
            "transforms must refresh percentage displays without changing output margins");
    document.replace(first, image({500, 250}), QStringLiteral("replacement"));
    require(border->value() == 8 && spacing->value() == 4,
            "replacement images must refresh the percentage reference");
    document.clear();
    require(!border->isEnabled() && !spacing->isEnabled() && !border->hasValue() &&
                !spacing->hasValue() && borderUnit->isEnabled() && spacingUnit->isEnabled() &&
                document.settings() == settings,
            "empty collages must preserve pixels and suspend percentage editing");
    spacingUnit->setCurrentValue(QStringLiteral("pixels"));
    require(spacing->isEnabled() && spacing->value() == 10,
            "pixel editing must remain available without images");
    document.append(image({200, 100}), QStringLiteral("new"));
    require(border->isEnabled() && border->value() == 20 && spacing->value() == 10,
            "new images must restore percentage editing with the saved pixel values");
    ImageStitchingDocument otherDocument;
    otherDocument.append(image({100, 100}), QStringLiteral("other"));
    ImageStitchingEditorWidget other(&otherDocument);
    auto* otherUnit = other.findChild<adqt::widgets::AdSegmented*>(
        QStringLiteral("collageOuterBorderUnitSegmented"));
    require(otherUnit->currentValue() == QStringLiteral("pixels") &&
                otherDocument.settings().border == 0,
            "unit choices and values must remain independent between editors");
}

void customGridPercentagesFollowCellWidth() {
    ImageStitchingDocument document;
    const auto first = document.append(image({200, 80}), QStringLiteral("first"));
    const auto second = document.append(image({400, 100}), QStringLiteral("second"));
    auto settings = document.settings();
    settings.direction = ImageStitchingDirection::Custom;
    settings.rows = 1;
    settings.columns = 2;
    settings.customGridInitialized = true;
    settings.border = 20;
    settings.spacing = 10;
    document.setSettings(settings);
    ImageStitchingEditorWidget editor(&document);
    auto* border =
        editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageOuterBorder"));
    auto* spacing =
        editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageImageSpacing"));
    auto* borderUnit = editor.findChild<adqt::widgets::AdSegmented*>(
        QStringLiteral("collageOuterBorderUnitSegmented"));
    auto* spacingUnit = editor.findChild<adqt::widgets::AdSegmented*>(
        QStringLiteral("collageImageSpacingUnitSegmented"));
    require(border && spacing && borderUnit && spacingUnit,
            "custom layout exposes border and spacing unit controls");
    borderUnit->setCurrentValue(QStringLiteral("percentage"));
    spacingUnit->setCurrentValue(QStringLiteral("percentage"));
    require(border->value() == 10 && spacing->value() == 5 &&
                border->toolTip() == QStringLiteral("Percentage of the grid cell width") &&
                spacing->toolTip() == border->toolTip(),
            "uniform Custom percentages must use the first transformed image's cell width");
    settings.sizing = ImageStitchingSizing::Original;
    document.setSettings(settings);
    require(border->value() == 5 && spacing->value() == 3,
            "original Custom percentages must use the widest transformed image's cell width");
    border->setValue(12);
    spacing->setValue(7);
    require(document.settings().border == 48 && document.settings().spacing == 28,
            "Custom percentage edits must convert against the grid cell width");
    document.rotate(first, true);
    document.remove(second);
    require(border->value() == 60 && spacing->value() == 35 && document.settings().border == 48 &&
                document.settings().spacing == 28,
            "changing Custom cell dimensions must refresh percentages and preserve saved pixels");
    document.clear();
    require(!border->isEnabled() && !spacing->isEnabled(),
            "empty Custom layouts must suspend percentage editing");
    document.append(image({100, 500}), QStringLiteral("replacement"));
    require(border->isEnabled() && spacing->isEnabled() && border->value() == 48 &&
                spacing->value() == 28,
            "Custom percentages must resume from the new cell width");
}

void customGridDimensionChangesFitToCanvas() {
    ImageStitchingDocument document;
    document.append(image({800, 400}), QStringLiteral("first"));
    const auto selected = document.append(image({400, 800}), QStringLiteral("second"));
    document.append(image({200, 200}), QStringLiteral("third"));
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 700);
    editor.show();
    pump();
    auto* direction =
        editor.findChild<adqt::widgets::AdSegmented*>(QStringLiteral("collageDirection"));
    auto* rows = editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageGridRows"));
    auto* columns =
        editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageGridColumns"));
    require(direction && rows && columns, "Custom exposes its dimension editors");
    direction->setCurrentValue(QStringLiteral("custom"));
    document.select(selected);
    QStringList edits;
    QObject::connect(&editor, &ImageStitchingEditorWidget::settingsEdited, &editor,
                     [&](const QString& field) { edits.append(field); });
    const auto editAndVerifyFit = [&](adqt::widgets::AdInputNumber* input, double value) {
        wheel(editor.canvas(), {120, 100}, 480);
        mouse(editor.canvas(), QEvent::MouseButtonPress, {50, 50}, Qt::MiddleButton,
              Qt::MiddleButton);
        mouse(editor.canvas(), QEvent::MouseMove, {140, 125}, Qt::NoButton, Qt::MiddleButton);
        mouse(editor.canvas(), QEvent::MouseButtonRelease, {140, 125}, Qt::MiddleButton,
              Qt::NoButton);
        input->setValue(value);
        pump();
        const auto zoom = editor.canvas()->zoom();
        const auto offset = editor.canvas()->offset();
        require(document.selectedId() == selected,
                "editing custom dimensions must retain the selected image");
        for (const auto& item : document.items())
            require(QRectF(editor.canvas()->rect())
                        .contains(editor.canvas()->imageViewportRect(item.id)),
                    "editing custom dimensions must fit every image to the canvas");
        editor.canvas()->fitToCanvas();
        require(qFuzzyCompare(editor.canvas()->zoom(), zoom) &&
                    (editor.canvas()->offset() - offset).manhattanLength() < .001,
                "automatic grid fitting must match the Fit to canvas action");
    };
    const auto initial = document.settings();
    require(initial.rows == 2 && initial.columns == 2,
            "three images initialize a two-by-two custom grid");
    editAndVerifyFit(columns, 1);
    require(columns->value() == 2 && document.settings() == initial,
            "a normalized no-op column edit must rebound and refit the unchanged grid");
    for (auto* input : {rows, columns})
        editAndVerifyFit(input, 3);
    require(edits.contains(QStringLiteral("rows")) && edits.contains(QStringLiteral("columns")),
            "custom dimension edits must publish their persistence fields");
}

void detailTilesRemainBoundedAndOriginalsUnchanged() {
    ImageStitchingDocument document;
    const auto source = image({3840, 2160}, QColor(20, 40, 60, 110));
    const auto key = source.cacheKey();
    document.append(source, QStringLiteral("4K"));
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1200, 800);
    editor.show();
    pump();
    wheel(editor.canvas(), {300, 250}, 240);
    QElapsedTimer timer;
    timer.start();
    while (editor.canvas()->property("detailTileCount").toInt() == 0 && timer.elapsed() < 10000) {
        pump();
        QThread::msleep(5);
    }
    require(editor.canvas()->property("detailTileCount").toInt() > 0,
            "zoomed 4K preview must obtain worker-generated detail");
    require(editor.canvas()->property("detailPreviewBytes").toLongLong() <= 64 * 1024 * 1024,
            "detail previews must remain within the window cache budget");
    QImage expected(1, 1, QImage::Format_ARGB32_Premultiplied);
    expected.fill(Qt::white);
    {
        QPainter painter(&expected);
        painter.fillRect(expected.rect(), QColor(20, 40, 60, 110));
    }
    const auto rendered = editor.canvas()->grab().toImage();
    const auto sample = rendered.pixelColor(rendered.width() / 2, rendered.height() / 2);
    require(std::abs(sample.red() - expected.pixelColor(0, 0).red()) <= 2 &&
                std::abs(sample.blue() - expected.pixelColor(0, 0).blue()) <= 2,
            "detail overlay must compose semitransparent pixels exactly once");
    require(source.cacheKey() == key && document.items()[0].image.cacheKey() == key,
            "preview interactions must preserve original raster storage");
    const auto before = editor.canvas()->zoom();
    document.rotate(document.items()[0].id, true);
    require(qFuzzyCompare(editor.canvas()->zoom(), before), "rotation must preserve preview zoom");
}

void backgroundEditsPreservePendingNumericInput() {
    ImageStitchingDocument document;
    document.append(image({500, 300}), QStringLiteral("first"));
    document.append(image({500, 300}), QStringLiteral("second"));
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 650);
    editor.show();
    pump();
    auto* border =
        editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageOuterBorder"));
    auto* input = border->findChild<QLineEdit*>();
    require(input != nullptr, "border editor must expose its numeric input");
    input->setFocus(Qt::OtherFocusReason);
    input->setText(QStringLiteral("42"));
    const auto zoom = editor.canvas()->zoom();
    const auto offset = editor.canvas()->offset();
    auto settings = document.settings();
    settings.background = QColor(20, 60, 100, 70);
    document.setSettings(settings);
    pump();
    require(input->text().startsWith(QStringLiteral("42")) && input->hasFocus() &&
                document.settings().border == 0,
            "background edits must preserve unrelated uncommitted numeric input and focus");
    require(qFuzzyCompare(editor.canvas()->zoom(), zoom) &&
                (editor.canvas()->offset() - offset).manhattanLength() < .001,
            "background edits must preserve viewport geometry");
    input->clearFocus();
    pump();
    require(document.settings().border == 42,
            "the preserved numeric edit must still commit through the normal control workflow");
}

void thumbnailReorderingPreservesWidgetsAndFocus() {
    ImageStitchingDocument document;
    for (int index = 0; index < 9; ++index)
        document.append(image({150, 100}), QString::number(index));
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1200, 700);
    editor.show();
    pump();
    auto* strip = editor.findChild<QWidget*>(QStringLiteral("collageThumbnails"));
    auto* focused = strip->layout()->itemAt(4)->widget();
    focused->setFocus(Qt::OtherFocusReason);
    const auto first = document.items().front().id;
    const auto last = document.items().back().id;
    editor.canvas()->centerImage(first);
    const auto anchor = editor.canvas()->imageViewportRect(first).center();
    require(document.swapItems(first, last), "thumbnail order fixture swaps distant items");
    require((editor.canvas()->imageViewportRect(first).center() - anchor).manhattanLength() < .001,
            "reordering equal-size images must preserve the selected viewport anchor");
    const auto verifyOrder = [&] {
        require(strip->layout()->count() == document.items().size(),
                "thumbnail strip contains exactly the surviving document items");
        for (int index = 0; index < strip->layout()->count(); ++index) {
            auto* thumbnail = strip->layout()->itemAt(index)->widget();
            require(thumbnail->property("collageItemId").toULongLong() ==
                            document.items()[index].id &&
                        thumbnail->accessibleName().startsWith(
                            QStringLiteral("Image %1:").arg(index + 1)),
                    "thumbnail layout and accessible ordinals follow the document order");
        }
    };
    verifyOrder();
    require(strip->layout()->itemAt(4)->widget() == focused && focused->hasFocus(),
            "swapping distant thumbnails retains the unaffected widget and keyboard focus");
    document.remove(last);
    verifyOrder();
    require(focused->hasFocus(), "removing another thumbnail retains keyboard focus");
}

void detailTilesPreserveColorOrientationAndCroppedPalette() {
    for (const auto format : {QImage::Format_RGBA64_Premultiplied, QImage::Format_Mono}) {
        ImageStitchingDocument document;
        QImage source(1603, 1201, format);
        require(!source.isNull(), "detail color fixture allocation");
        const std::array<QColor, 4> colors{QColor(190, 40, 20, 110), QColor(20, 180, 50, 150),
                                           QColor(20, 60, 190, 200), QColor(190, 140, 20, 90)};
        if (format == QImage::Format_Mono) {
            source.setColorTable({qRgb(255, 255, 255), qRgb(0, 0, 0)});
            for (int y = 0; y < source.height(); ++y)
                for (int x = 0; x < source.width(); ++x)
                    source.setPixel(x, y, unsigned((x / 200 + y / 200) % 2));
        } else {
            source.setColorSpace(QColorSpace::DisplayP3);
            QPainter painter(&source);
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            for (int index = 0; index < 4; ++index)
                painter.fillRect(QRect(index % 2 * 802, index / 2 * 601, 802, 601),
                                 colors[size_t(index)]);
        }
        const auto key = source.cacheKey();
        const auto id = document.append(source, QStringLiteral("color and palette"));
        require(id != 0, "detail color fixture admission");
        ImageStitchingEditorWidget editor(&document);
        editor.resize(1200, 800);
        editor.show();
        document.select(0);
        pump();
        wheel(editor.canvas(), QRectF(editor.canvas()->rect()).center(), 300);
        for (int orientation = 0; orientation < 3; ++orientation) {
            if (orientation == 1)
                require(document.rotate(id, true), "rotate detail color fixture");
            if (orientation == 2)
                require(document.flip(id, true), "flip detail color fixture");
            QElapsedTimer timer;
            timer.start();
            while ((editor.canvas()->property("detailTileCount").toInt() == 0 ||
                    ScreenshotExportCoordinator::shared().pendingJobCount() != 0) &&
                   timer.elapsed() < 10000) {
                pump();
                QThread::msleep(5);
            }
            require(editor.canvas()->property("detailTileCount").toInt() > 0,
                    "cropped high-depth and palette previews must obtain detail tiles");
            const auto frame = editor.canvas()->grab().toImage();
            const auto& item = document.items().front();
            const auto sourceBounds = item.transform.mapRect(QRectF(QPointF{}, source.size()));
            const auto bounds = editor.canvas()->imageViewportRect(id);
            int checked = 0;
            for (const QPoint point :
                 {QPoint(300, 300), QPoint(1100, 300), QPoint(300, 900), QPoint(1100, 900)}) {
                const QPointF transformed =
                    item.transform.map(QPointF(point)) - sourceBounds.topLeft();
                const QPointF screen =
                    bounds.topLeft() +
                    QPointF(transformed.x() / sourceBounds.width() * bounds.width(),
                            transformed.y() / sourceBounds.height() * bounds.height());
                if (!QRectF(editor.canvas()->rect()).adjusted(10, 10, -10, -10).contains(screen))
                    continue;
                QImage pixel(1, 1, format == QImage::Format_Mono ? QImage::Format_RGB32 : format);
                pixel.fill(source.pixelColor(point));
                if (source.colorSpace().isValid()) {
                    pixel.setColorSpace(source.colorSpace());
                    pixel = pixel.convertedToColorSpace(QColorSpace::SRgb);
                }
                QImage expected(1, 1, QImage::Format_ARGB32_Premultiplied);
                expected.fill(Qt::white);
                {
                    QPainter painter(&expected);
                    painter.drawImage(QPoint{}, pixel);
                }
                const auto actual = frame.pixelColor((screen * frame.devicePixelRatio()).toPoint());
                const auto color = expected.pixelColor(0, 0);
                const auto diagnostic =
                    QStringLiteral("detail color mismatch: format=%1 orientation=%2 "
                                   "source=(%3,%4) screen=(%5,%6) actual=%7 expected=%8")
                        .arg(int(format))
                        .arg(orientation)
                        .arg(point.x())
                        .arg(point.y())
                        .arg(screen.x())
                        .arg(screen.y())
                        .arg(actual.name(QColor::HexArgb))
                        .arg(color.name(QColor::HexArgb));
                require(std::abs(actual.red() - color.red()) <= 3 &&
                            std::abs(actual.green() - color.green()) <= 3 &&
                            std::abs(actual.blue() - color.blue()) <= 3,
                        qPrintable(diagnostic));
                ++checked;
            }
            require(checked >= 2,
                    "detail color checks must exercise multiple visible source areas");
        }
        require(source.cacheKey() == key && document.items().front().image.cacheKey() == key,
                "cropped detail tiles must preserve high-depth and palette originals");
    }
}

void stronglyReducedDetailsKeepNativeAreaQuality() {
    QImage source(2053, 1543, QImage::Format_RGBA64_Premultiplied);
    require(!source.isNull(), "area-detail fixture allocation");
    for (int y = 0; y < source.height(); ++y) {
        auto* pixels = reinterpret_cast<QRgba64*>(source.scanLine(y));
        for (int x = 0; x < source.width(); ++x)
            pixels[x] = QRgba64::fromRgba(static_cast<quint8>((x * 53 + y * 23) % 256),
                                          static_cast<quint8>((x * 11 + y * 71) % 256),
                                          static_cast<quint8>((x * 101 + y * 31) % 256),
                                          static_cast<quint8>(31 + (x * 19 + y * 7) % 225))
                            .premultiplied();
    }
    source.setColorSpace(QColorSpace::DisplayP3);
    const auto key = source.cacheKey();
    const auto converted = source.convertedToColorSpace(QColorSpace::SRgb);
    ImageStitchingDocument document;
    const auto id = document.append(source, QStringLiteral("high-frequency alpha colors"));
    require(id != 0, "area-detail fixture admission");
    ImageStitchingEditorWidget editor(&document);
    editor.resize(800, 640);
    editor.show();
    document.select(0);
    pump();
    QElapsedTimer timer;
    timer.start();
    while ((editor.canvas()->property("detailTileCount").toInt() == 0 ||
            ScreenshotExportCoordinator::shared().pendingJobCount() != 0) &&
           timer.elapsed() < 10000) {
        pump();
        QThread::msleep(1);
    }
    require(editor.canvas()->property("detailTileCount").toInt() > 0,
            "strongly reduced detail must finish");
    const auto bounds = editor.canvas()->imageViewportRect(id);
    const auto actual = editor.canvas()->grab().toImage();
    const auto dpr = actual.devicePixelRatio();
    const qreal desiredScale = std::min(
        1.0, std::max(bounds.width() / source.width(), bounds.height() / source.height()) * dpr);
    const QSize wanted(qCeil(source.width() * desiredScale), qCeil(source.height() * desiredScale));
    const qreal scale =
        std::min(qreal(wanted.width()) / source.width(), qreal(wanted.height()) / source.height());
    const QSize tileSize(qCeil(source.width() * scale), qCeil(source.height() * scale));
    const qint64 tileBytes = qint64(tileSize.width()) * tileSize.height() * 4;
    require(editor.canvas()->property("detailPreviewBytes").toLongLong() -
                    document.items().front().preview.sizeInBytes() ==
                tileBytes,
            "full-fit quality reference must use the completed detail tile's actual resolution");
    require(tileSize.width() <= source.width() / 4 && tileSize.height() <= source.height() / 4,
            "area quality fixture must shrink both dimensions by at least four");
    const auto tile = converted.scaled(tileSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage expected(actual.size(), QImage::Format_ARGB32_Premultiplied);
    expected.fill(Qt::white);
    {
        QPainter painter(&expected);
        painter.scale(dpr, dpr);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawImage(bounds, tile);
    }
    for (int y = 1; y < 8; ++y)
        for (int x = 1; x < 10; ++x) {
            const auto position =
                ((bounds.topLeft() + QPointF(bounds.width() * x / 10, bounds.height() * y / 8)) *
                 dpr)
                    .toPoint();
            const auto before = expected.pixelColor(position);
            const auto after = actual.pixelColor(position);
            if (std::abs(before.red() - after.red()) > 2 ||
                std::abs(before.green() - after.green()) > 2 ||
                std::abs(before.blue() - after.blue()) > 2)
                std::cerr << "area detail pixel=" << position.x() << ',' << position.y()
                          << " expected=" << before.name(QColor::HexArgb).toStdString()
                          << " actual=" << after.name(QColor::HexArgb).toStdString()
                          << " bounds=" << bounds.x() << ',' << bounds.y() << ',' << bounds.width()
                          << ',' << bounds.height() << " wanted=" << wanted.width() << 'x'
                          << wanted.height() << " tile=" << tileSize.width() << 'x'
                          << tileSize.height() << " dpr=" << dpr << " previewBytes="
                          << editor.canvas()->property("detailPreviewBytes").toLongLong()
                          << " baseBytes=" << document.items().front().preview.sizeInBytes()
                          << " source=" << source.width() * x / 10 << ',' << source.height() * y / 8
                          << '\n';
            require(std::abs(before.red() - after.red()) <= 2 &&
                        std::abs(before.green() - after.green()) <= 2 &&
                        std::abs(before.blue() - after.blue()) <= 2,
                    "bounded area details must retain native Qt edge, alpha and ICC averaging");
        }
    require(source.cacheKey() == key && document.items().front().image.cacheKey() == key,
            "area filtering must preserve immutable original pixels");
}

void cancelledDetailsRetainPreviewAccountingUntilRetirement() {
    struct Blockers final {
        std::atomic_int started{0};
        std::atomic_bool release{false};
    };
    auto blockers = std::make_shared<Blockers>();
    struct Release final {
        std::shared_ptr<Blockers> blockers;
        ~Release() {
            blockers->release.store(true);
        }
    } release{blockers};
    QObject receiver;
    const int workers = std::clamp(QThread::idealThreadCount(), 1, 2);
    for (int index = 0; index < workers; ++index) {
        const auto job = ScreenshotExportCoordinator::shared().submit(
            &receiver, ScreenshotExportCoordinator::Priority::Foreground,
            [blockers](const ScreenshotExportCancellation& cancellation) {
                blockers->started.fetch_add(1);
                while (!blockers->release.load() && !cancellation.isCancellationRequested())
                    QThread::msleep(1);
                return ScreenshotExportTaskResult{};
            },
            [](const ScreenshotExportTaskResult&) {});
        require(job.isValid(), "detail retirement fixture worker admission");
    }
    QElapsedTimer timer;
    timer.start();
    while (blockers->started.load() != workers && timer.elapsed() < 10000) {
        pump();
        QThread::msleep(1);
    }
    require(blockers->started.load() == workers, "detail retirement fixture workers must start");
    ImageStitchingDocument document;
    document.append(image({2000, 1600}), QStringLiteral("queued detail"));
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1200, 800);
    editor.show();
    timer.restart();
    while (editor.canvas()->property("detailPendingCount").toInt() == 0 &&
           timer.elapsed() < 10000) {
        pump();
        QThread::msleep(1);
    }
    require(editor.canvas()->property("detailPendingCount").toInt() == 1,
            "detail retirement fixture must hold one queued preview");
    document.clear();
    require(editor.canvas()->property("detailPendingCount").toInt() == 1 &&
                editor.canvas()->property("detailPreviewBytes").toLongLong() > 0,
            "cancelling a detail keeps its reserved preview accounted until completion retires");
    blockers->release.store(true);
    timer.restart();
    while ((ScreenshotExportCoordinator::shared().pendingJobCount() != 0 ||
            editor.canvas()->property("detailPendingCount").toInt() != 0) &&
           timer.elapsed() < 10000) {
        pump();
        QThread::msleep(1);
    }
    require(editor.canvas()->property("detailPendingCount").toInt() == 0 &&
                editor.canvas()->property("detailPreviewBytes").toLongLong() == 0,
            "retired cancelled detail releases its window preview reservation");
}

void languageChangesPreserveSettingsAndRefreshLabels() {
    ImageStitchingDocument document;
    document.append(image({120, 100}), QStringLiteral("sample"));
    ImageStitchingEditorWidget editor(&document);
    editor.resize(1000, 650);
    editor.show();
    pump();
    auto* border =
        editor.findChild<adqt::widgets::AdInputNumber*>(QStringLiteral("collageOuterBorder"));
    auto* borderUnit = editor.findChild<adqt::widgets::AdSegmented*>(
        QStringLiteral("collageOuterBorderUnitSegmented"));
    borderUnit->setCurrentValue(QStringLiteral("percentage"));
    border->setValue(12.5);
    auto customSettings = document.settings();
    customSettings.direction = ImageStitchingDirection::Custom;
    customSettings.rows = 2;
    customSettings.columns = 3;
    customSettings.customGridInitialized = true;
    document.setSettings(customSettings);
    const auto settings = document.settings();
    QTranslator translator;
    require(translator.load(QStringLiteral(":/i18n/snow_shot_zh_CN.qm")),
            "collage Chinese translation catalog must be available");
    QApplication::installTranslator(&translator);
    pump();
    const auto localized = QCoreApplication::translate("ImageStitchingEditorWidget", "Add images");
    require(localized != QStringLiteral("Add images") &&
                button(&editor, "collageAddImages")->toolTip() == localized &&
                button(&editor, "collageAddImages")->accessibleName() == localized,
            "language change must refresh cached editor labels");
    auto* prompt = editor.findChild<QLabel*>(QStringLiteral("collageEmptyPrompt"));
    auto* guide = button(&editor, "collageEmptyAddImages");
    require(prompt &&
                prompt->text() ==
                    QCoreApplication::translate("ImageStitchingEditorWidget",
                                                "Add images to start image stitching") &&
                guide->text() == localized && guide->accessibleName() == localized,
            "language change must refresh the empty prompt and accessible guide button");
    require(
        button(editor.canvas(), "collageRotateClockwise")->accessibleName() ==
            QCoreApplication::translate("ImageStitchingEditorWidget", "Rotate clockwise by 90°"),
        "language change must refresh floating toolbar accessibility");
    auto* direction =
        editor.findChild<adqt::widgets::AdSegmented*>(QStringLiteral("collageDirection"));
    auto* sizing = editor.findChild<adqt::widgets::AdSegmented*>(QStringLiteral("collageSizing"));
    require(direction && sizing &&
                direction->optionLabel(1) ==
                    QCoreApplication::translate("ImageStitchingEditorWidget", "Horizontal") &&
                direction->optionLabel(2) ==
                    QCoreApplication::translate("ImageStitchingEditorWidget", "Custom") &&
                sizing->optionLabel(1) ==
                    QCoreApplication::translate("ImageStitchingEditorWidget", "Original size"),
            "language change must refresh segmented choices");
    for (const auto* source : {"Rows", "Columns"}) {
        const auto name = QStringLiteral("collageGrid%1").arg(QString::fromLatin1(source));
        auto* input = editor.findChild<adqt::widgets::AdInputNumber*>(name);
        auto* label = editor.findChild<QLabel*>(name + QStringLiteral("Label"));
        const auto localizedName =
            QCoreApplication::translate("ImageStitchingEditorWidget", source);
        require(input && label && localizedName != QString::fromLatin1(source) &&
                    input->accessibleName() == localizedName && label->text() == localizedName,
                "language change must refresh custom grid labels and accessible names");
    }
    for (const auto* name :
         {"collageOuterBorderUnitSegmented", "collageImageSpacingUnitSegmented"}) {
        auto* unit = editor.findChild<adqt::widgets::AdSegmented*>(QString::fromLatin1(name));
        require(unit && unit->optionLabel(0) != QStringLiteral("Pixels") &&
                    unit->optionLabel(0) ==
                        QCoreApplication::translate("ImageStitchingEditorWidget", "Pixels") &&
                    unit->optionLabel(1) ==
                        QCoreApplication::translate("ImageStitchingEditorWidget", "Percentage"),
                "language change must refresh both unit switches");
    }
    require(
        borderUnit->currentValue() == QStringLiteral("percentage") && border->value() == 13 &&
            border->decimals() == 0 && border->suffixText() == QStringLiteral("%") &&
            border->accessibleName() == QCoreApplication::translate("ImageStitchingEditorWidget",
                                                                    "Outer border in percentage") &&
            border->toolTip() == QCoreApplication::translate("ImageStitchingEditorWidget",
                                                             "Percentage of the grid cell width"),
        "language changes must preserve active units and refresh their accessibility and help");
    auto* fit = button(editor.canvas(), "collageFitToCanvas");
    require(fit->text().isEmpty() &&
                fit->toolTip() ==
                    QCoreApplication::translate("ImageStitchingEditorWidget", "Fit to canvas") &&
                fit->accessibleName() == fit->toolTip(),
            "language change must refresh the floating fit tooltip and accessible name");
    require(document.settings() == settings, "language change must not edit layout settings");
    QApplication::removeTranslator(&translator);
    pump();
    require(button(&editor, "collageAddImages")->toolTip() == QStringLiteral("Add images"),
            "return to source language must refresh cached labels");
}

QImage previewFixture(QSize size, int variant) {
    QImage result(size, QImage::Format_ARGB32_Premultiplied);
    result.fill(variant == 1 ? QColor(Qt::transparent) : QColor(248, 250, 253));
    result.setColorSpace(QColorSpace::SRgb);
    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRect panel = result.rect().adjusted(variant == 1 ? 14 : 0, variant == 1 ? 14 : 0,
                                               variant == 1 ? -14 : 0, variant == 1 ? -14 : 0);
    painter.setBrush(Qt::white);
    painter.setPen(QColor(226, 232, 240));
    painter.drawRoundedRect(panel, 12, 12);
    painter.fillRect(QRect(panel.left() + 1, panel.top() + 1, panel.width() - 2, 50),
                     QColor(241, 245, 249));
    painter.setPen(QColor(30, 41, 59));
    QFont heading = painter.font();
    heading.setPixelSize(20);
    heading.setWeight(QFont::DemiBold);
    painter.setFont(heading);
    painter.drawText(panel.adjusted(22, 12, -22, -12), Qt::AlignLeft | Qt::AlignTop,
                     variant == 0   ? QStringLiteral("Screenshot history")
                     : variant == 1 ? QStringLiteral("Capture details")
                                    : QStringLiteral("Activity overview"));
    painter.setPen(Qt::NoPen);
    if (variant == 0) {
        for (int i = 0; i < 3; ++i) {
            const QRect card(panel.left() + 24 + i * 218, panel.top() + 78, 196, 166);
            painter.setBrush(QColor(239 - i * 5, 246 - i * 5, 255));
            painter.drawRoundedRect(card, 8, 8);
            painter.setBrush(QColor(186, 215, 255));
            painter.drawRoundedRect(card.adjusted(16, 20, -16, -62), 4, 4);
            painter.setBrush(QColor(148, 163, 184));
            painter.drawRoundedRect(QRect(card.left() + 16, card.bottom() - 39, 125, 7), 3, 3);
            painter.drawRoundedRect(QRect(card.left() + 16, card.bottom() - 23, 85, 6), 3, 3);
        }
    } else if (variant == 1) {
        for (int i = 0; i < 4; ++i) {
            const int top = panel.top() + 82 + i * 55;
            painter.setBrush(QColor(226, 232, 240));
            painter.drawRoundedRect(QRect(panel.left() + 24, top, 110, 8), 3, 3);
            painter.setBrush(i == 1 ? QColor(186, 224, 255) : QColor(248, 250, 252));
            painter.drawRoundedRect(QRect(panel.left() + 160, top - 7, panel.width() - 186, 28), 5,
                                    5);
        }
        painter.setBrush(QColor(22, 119, 255));
        painter.drawRoundedRect(QRect(panel.right() - 120, panel.bottom() - 54, 96, 32), 5, 5);
    } else {
        painter.setPen(QPen(QColor(22, 119, 255), 4));
        QPolygonF line;
        for (int i = 0; i < 10; ++i)
            line << QPointF(panel.left() + 30 + i * 91, panel.top() + 156 - (i * 13 % 72));
        painter.drawPolyline(line);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(148, 163, 184));
        for (int i = 0; i < 4; ++i)
            painter.drawRoundedRect(QRect(panel.left() + 30 + i * 200, panel.bottom() - 34, 110, 6),
                                    3, 3);
    }
    return result;
}

int renderPreview(const QString& path, bool dark, bool percentage, bool empty) {
    adqt::theme::ThemeManager::instance().setPreset(dark ? adqt::theme::ThemeScheme::Dark
                                                         : adqt::theme::ThemeScheme::Light);
    ImageStitchingDocument document;
    const auto first =
        document.append(previewFixture({720, 280}, 0), QStringLiteral("History screenshot"));
    document.append(previewFixture({480, 390}, 1), QStringLiteral("Capture settings"));
    document.append(previewFixture({900, 240}, 2), QStringLiteral("Activity graph"));
    auto settings = document.settings();
    settings.border = 16;
    settings.spacing = 16;
    settings.background = QColor(255, 255, 255, 90);
    if (QApplication::arguments().contains(QStringLiteral("--custom")))
        settings.direction = ImageStitchingDirection::Custom;
    document.setSettings(settings);
    if (empty)
        document.clear();
    ImageStitchingEditorWidget editor(&document);
    if (percentage)
        for (const auto* name :
             {"collageOuterBorderUnitSegmented", "collageImageSpacingUnitSegmented"})
            editor.findChild<adqt::widgets::AdSegmented*>(QString::fromLatin1(name))
                ->setCurrentValue(QStringLiteral("percentage"));
    editor.resize(1200, 700);
    editor.setAutoFillBackground(true);
    editor.show();
    document.select(first);
    if (QApplication::arguments().contains(QStringLiteral("--preview-actions")) && !empty) {
        editor.findChild<QWidget*>(QStringLiteral("collageThumbnail_%1").arg(first))->setFocus();
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 400) {
        pump();
        QThread::msleep(5);
    }
    QMimeData dragFiles;
    if (QApplication::arguments().contains(QStringLiteral("--drag"))) {
        dragFiles.setUrls(
            {QUrl::fromLocalFile(QDir::temp().filePath(QStringLiteral("image.png")))});
        QDragEnterEvent enter(QRectF(editor.rect()).center(), Qt::CopyAction, &dragFiles,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&editor, &enter);
        pump();
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    const bool saved = editor.grab().save(path);
    ScreenshotExportCoordinator::shared().shutdown();
    return saved ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    adqt::theme::ThemeManager::instance().applyTo(application);
    const auto arguments = QApplication::arguments();
    const auto preview = arguments.indexOf(QStringLiteral("--preview"));
    try {
        TemporaryApplicationStorage temporaryStorage;
        if (preview >= 0 && preview + 1 < arguments.size())
            return renderPreview(arguments[preview + 1],
                                 arguments.contains(QStringLiteral("--dark")),
                                 arguments.contains(QStringLiteral("--percentage")),
                                 arguments.contains(QStringLiteral("--empty")));
        if (arguments.contains(QStringLiteral("--drag-drop-only"))) {
            externalImageDrops();
            releaseOnlySwapAndCrossWindowIsolation();
            ScreenshotExportCoordinator::shared().shutdown();
            std::cout << "Collage drag-and-drop tests passed\n";
            return 0;
        }
        if (arguments.contains(QStringLiteral("--empty-state-only"))) {
            controlsEmptyBusyAndPresets();
            languageChangesPreserveSettingsAndRefreshLabels();
            ScreenshotExportCoordinator::shared().shutdown();
            std::cout << "Collage empty state tests passed\n";
            return 0;
        }
        if (arguments.contains(QStringLiteral("--static-editing-only"))) {
            centeredStaticResizeMoveAndCancellation();
            staticPlacementOverflowAndDetailAlpha();
            ScreenshotExportCoordinator::shared().shutdown();
            std::cout << "Static stitching editing tests passed\n";
            return 0;
        }
        if (arguments.contains(QStringLiteral("--selection-outline-only"))) {
            selectionOutlineMatchesPdfHandles();
            replacementHighlightMatchesDrawEngineMarquee();
            ScreenshotExportCoordinator::shared().shutdown();
            std::cout << "Collage selection outline tests passed\n";
            return 0;
        }
        if (arguments.contains(QStringLiteral("--form-controls-only"))) {
            backgroundColorsShareRowAndCenteredSwatches();
            controlsEmptyBusyAndPresets();
            spacingUnitsPreservePixelsAndConvertEdits();
            spacingPercentagesFollowImageReference();
            customGridPercentagesFollowCellWidth();
            customGridDimensionChangesFitToCanvas();
            languageChangesPreserveSettingsAndRefreshLabels();
            ScreenshotExportCoordinator::shared().shutdown();
            std::cout << "Collage form control tests passed\n";
            return 0;
        }
        if (arguments.contains(QStringLiteral("--spacing-units-only"))) {
            spacingUnitsPreservePixelsAndConvertEdits();
            spacingPercentagesFollowImageReference();
            customGridPercentagesFollowCellWidth();
            languageChangesPreserveSettingsAndRefreshLabels();
            ScreenshotExportCoordinator::shared().shutdown();
            std::cout << "Collage spacing unit tests passed\n";
            return 0;
        }
        if (arguments.contains(QStringLiteral("--thumbnail-scrollbar-only"))) {
            thumbnailScrollBarOverlaysTheList();
            ScreenshotExportCoordinator::shared().shutdown();
            std::cout << "Collage thumbnail scrollbar tests passed\n";
            return 0;
        }
        if (arguments.contains(QStringLiteral("--thumbnail-actions-only"))) {
            thumbnailActionsUseTranslucentOverlays();
            floatingBottomToolbarControlsCollection();
            ScreenshotExportCoordinator::shared().shutdown();
            std::cout << "Collage thumbnail action tests passed\n";
            return 0;
        }
        if (arguments.contains(QStringLiteral("--floating-toolbar-only"))) {
            floatingBottomToolbarControlsCollection();
            ScreenshotExportCoordinator::shared().shutdown();
            std::cout << "Collage floating bottom toolbar tests passed\n";
            return 0;
        }
        if (arguments.contains(QStringLiteral("--fit-control-only"))) {
            floatingFitToCanvasFollowsViewport();
            arrangementChangesFitToCanvas();
            customGridDimensionChangesFitToCanvas();
            invalidLayoutKeepsSelectionAndTransformsUsable();
            languageChangesPreserveSettingsAndRefreshLabels();
            ScreenshotExportCoordinator::shared().shutdown();
            std::cout << "Collage fit control tests passed\n";
            return 0;
        }
        selectionOutlineMatchesPdfHandles();
        centeredStaticResizeMoveAndCancellation();
        staticPlacementOverflowAndDetailAlpha();
        externalImageDrops();
        replacementHighlightMatchesDrawEngineMarquee();
        thumbnailScrollBarOverlaysTheList();
        thumbnailActionsUseTranslucentOverlays();
        floatingBottomToolbarControlsCollection();
        floatingFitToCanvasFollowsViewport();
        arrangementChangesFitToCanvas();
        fitPanZoomAndNavigation();
        releaseOnlySwapAndCrossWindowIsolation();
        invalidLayoutKeepsSelectionAndTransformsUsable();
        backgroundColorsShareRowAndCenteredSwatches();
        controlsEmptyBusyAndPresets();
        spacingUnitsPreservePixelsAndConvertEdits();
        spacingPercentagesFollowImageReference();
        customGridPercentagesFollowCellWidth();
        customGridDimensionChangesFitToCanvas();
        detailTilesRemainBoundedAndOriginalsUnchanged();
        backgroundEditsPreservePendingNumericInput();
        thumbnailReorderingPreservesWidgetsAndFocus();
        detailTilesPreserveColorOrientationAndCroppedPalette();
        stronglyReducedDetailsKeepNativeAreaQuality();
        cancelledDetailsRetainPreviewAccountingUntilRetirement();
        languageChangesPreserveSettingsAndRefreshLabels();
        ScreenshotExportCoordinator::shared().shutdown();
        std::cout << "Collage editor tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
