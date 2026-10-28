#include "physical_key_test_support.h"
#include "snow_shot/presentation/canvasstatusreadout.h"
#include "snow_shot/presentation/screenshotgeometry.h"
#include "snow_shot/presentation/screenshotocrpresentation.h"
#include "snow_shot/presentation/screenshotrecognitionwindow.h"
#include "snow_shot/presentation/crosshaircursor.h"
#include "snow_shot/presentation/screenshotlatexrenderer.h"
#include "snow_shot/presentation/screenshottableeditor.h"
#include "snow_shot/presentation/windowshortcutmanager.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "theme/theme_manager.h"
#include "widgets/context_menu.h"
#include "widgets/input_text_edit.h"
#include "widgets/message.h"
#include "widgets/scroll_area.h"
#include "widgets/spin.h"

#include <QApplication>
#include <QClipboard>
#include <QColorSpace>
#include <QContextMenuEvent>
#include <QDir>
#include <QFileInfo>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFrame>
#include <QFontDatabase>
#include <QFocusEvent>
#include <QGuiApplication>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QScrollBar>
#include <QScreen>
#include <QScopeGuard>
#include <QStyleOptionGraphicsItem>
#include <QTextBlock>
#include <QTextLayout>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFragment>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QTranslator>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>
#include <QWheelEvent>

#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <functional>
#include <limits>
#include <utility>

#ifdef Q_OS_MACOS
#import <AppKit/AppKit.h>
#endif

#if defined(Q_OS_WIN) || defined(_WIN32)
#include <qt_windows.h>
#endif

namespace {
class LatexPreviewTranslator final : public QTranslator {
  public:
    bool isEmpty() const override {
        return false;
    }

    QString translate(const char* context, const char* source, const char*, int) const override {
        if (qstrcmp(context, "ScreenshotRecognitionWindow") == 0 ||
            (qstrcmp(context, "adqt::widgets::AdTextEdit") == 0 &&
             qstrcmp(source, "Multiline input") == 0))
            return QStringLiteral("Translated ") + QString::fromUtf8(source);
        return {};
    }
};

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void companionWheelZoomPreservesRecognition(ScreenshotOriginalImagePreviewWindow& preview,
                                            QWidget& owner, QTextDocument& document,
                                            bool bottomRight = false) {
    const QRect originalGeometry = preview.geometry();
    const QRect ownerGeometry = owner.geometry();
    const QString originalText = document.toPlainText();
    QWidget* const focus = QApplication::focusWidget();
    const auto wheel = [&](int angle) {
        const QPointF localPosition;
        QWheelEvent event(localPosition, QPointF(preview.mapToGlobal(QPoint())), QPoint(),
                          QPoint(0, angle), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        event.ignore();
        QApplication::sendEvent(&preview, &event);
        require(event.isAccepted(), "wheel input over the companion must be consumed");
    };
    wheel(120);
    require(preview.size() == QSize(qRound(originalGeometry.width() * 1.1),
                                    qRound(originalGeometry.height() * 1.1)) &&
                (bottomRight ? preview.geometry().bottomRight() == originalGeometry.bottomRight()
                             : preview.geometry().topLeft() == originalGeometry.topLeft()) &&
                owner.geometry() == ownerGeometry && document.toPlainText() == originalText &&
                QApplication::focusWidget() == focus && !preview.hasFocus(),
            "companion wheel zoom must preserve recognition geometry, text and focus");
    wheel(-120);
    require(preview.geometry() == originalGeometry,
            "reversing companion wheel zoom must return to the initial selection-sized preview");
    auto* readout = preview.findChild<CanvasStatusReadout*>();
    require(readout != nullptr, "the companion must use the shared zoom hint");
    auto* timer = readout->dismissalTimer();
    timer->stop();
    require(QMetaObject::invokeMethod(timer, "timeout", Qt::DirectConnection),
            "hiding the integration fixture zoom hint failed");
}

void clickCell(ScreenshotTableEditor& editor, int row, int column) {
    const QModelIndex index = editor.model()->index(row, column);
    const QPoint localPosition = editor.visualRect(index).center();
    const QPoint globalPosition = editor.viewport()->mapToGlobal(localPosition);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(localPosition), QPointF(globalPosition),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(editor.viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(localPosition), QPointF(globalPosition),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(editor.viewport(), &release);
    QApplication::processEvents();
}

QPolygonF ocrQuad(const QRectF& rect) {
    return {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
}

QList<QGraphicsItem*> ocrGlyphItems(QGraphicsView* layer) {
    QList<QGraphicsItem*> items;
    if (layer != nullptr && layer->scene() != nullptr) {
        for (auto* item : layer->scene()->items()) {
            if (item->zValue() == 0) {
                items.push_back(item);
            }
        }
    }
    return items;
}

bool doubleClickWidget(QWidget& receiver, const QPoint& point,
                       Qt::MouseButton button = Qt::LeftButton) {
    bool accepted = false;
    for (const auto type : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease,
                            QEvent::MouseButtonDblClick, QEvent::MouseButtonRelease}) {
        QMouseEvent event(type, QPointF(point), QPointF(receiver.mapToGlobal(point)), button,
                          type == QEvent::MouseButtonRelease ? Qt::NoButton : button,
                          Qt::NoModifier);
        QApplication::sendEvent(&receiver, &event);
        if (type == QEvent::MouseButtonDblClick) {
            accepted = event.isAccepted();
        }
    }
    return accepted;
}

void processEditorClose() {
    QApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

QAction* contextMenuAction(adqt::widgets::AdContextMenu& menu, const QString& text) {
    for (QAction* action : menu.actions()) {
        if (action != nullptr && action->text() == text) {
            return action;
        }
    }
    return nullptr;
}

QStringList contextMenuActionTexts(const adqt::widgets::AdContextMenu& menu) {
    QStringList texts;
    for (QAction* action : menu.actions()) {
        texts.push_back(action != nullptr && action->isSeparator() ? QStringLiteral("|")
                                                                   : action->text());
    }
    return texts;
}

void requireContextMenuShortcutsHidden(const adqt::widgets::AdContextMenu& menu) {
    for (QAction* action : menu.actions()) {
        require(action == nullptr || action->shortcut().isEmpty(),
                "recognition context actions should not show shortcuts");
    }
}

void inspectContextMenu(QWidget& receiver, const QPoint& localPosition,
                        const std::function<void(adqt::widgets::AdContextMenu&)>& inspect) {
    bool inspected = false;
    receiver.setFocus(Qt::OtherFocusReason);
    QApplication::processEvents();
    QTimer::singleShot(0, &receiver, [&]() {
        auto* menu = qobject_cast<adqt::widgets::AdContextMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr) {
            for (QWidget* widget : QApplication::topLevelWidgets()) {
                auto* candidate = qobject_cast<adqt::widgets::AdContextMenu*>(widget);
                if (candidate != nullptr && candidate->isVisible()) {
                    menu = candidate;
                    break;
                }
            }
        }
        if (menu == nullptr) {
            std::cerr << "missing Ant context menu for " << receiver.objectName().toStdString()
                      << '\n';
        }
        require(menu != nullptr, "context menu must use AdContextMenu");
        inspect(*menu);
        inspected = true;
        menu->close();
    });
    const QPoint globalPosition = receiver.mapToGlobal(localPosition);
    QContextMenuEvent event(QContextMenuEvent::Mouse, localPosition, globalPosition);
    QApplication::sendEvent(&receiver, &event);
    QApplication::processEvents();
    require(inspected && event.isAccepted(), "context menu event must be handled locally");
    QEventLoop settle;
    QTimer::singleShot(10, &settle, &QEventLoop::quit);
    settle.exec();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QImage renderTableEditor(ScreenshotTableEditor& editor, qreal devicePixelRatio) {
    const QSize pixelSize(qCeil(editor.width() * devicePixelRatio),
                          qCeil(editor.height() * devicePixelRatio));
    QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    editor.render(&painter);
    return image;
}

double averageLightness(const QImage& image) {
    if (image.isNull()) {
        return 0.0;
    }
    quint64 total = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            total += static_cast<quint64>(qGray(image.pixel(x, y)));
        }
    }
    return static_cast<double>(total) / static_cast<double>(image.width() * image.height());
}

QImage renderOcrHighlightingScene(ScreenshotOcrTextLayer& layer, qreal devicePixelRatio,
                                  const QColor& referenceMask = {},
                                  const QTransform& outputTransform = {},
                                  bool nativeReferenceMask = false) {
    const QRectF sceneRect = layer.sceneRect();
    QImage image(QSize(qCeil(sceneRect.width() * devicePixelRatio),
                       qCeil(sceneRect.height() * devicePixelRatio)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(devicePixelRatio);
    image.fill(QColor(214, 227, 239));
    QPainter painter(&image);
    painter.setTransform(outputTransform);
    painter.setClipRect(sceneRect, Qt::IntersectClip);
    if (referenceMask.isValid()) {
        QPainterPath complement;
        complement.addRect(sceneRect);
        for (const auto* item : ocrGlyphItems(&layer)) {
            if (!item->isVisible())
                continue;
            QPainterPath hole;
            hole.addPolygon(item->mapToScene(item->boundingRect()));
            hole.closeSubpath();
            // Sequential subtraction independently checks the union of overlapping holes,
            // including labels whose projected polygons have opposite winding.
            complement = complement.subtracted(hole);
        }
        painter.save();
        if (nativeReferenceMask) {
            QImage nativeMask(sceneRect.size().toSize(), QImage::Format_ARGB32_Premultiplied);
            nativeMask.fill(Qt::transparent);
            {
                QPainter maskPainter(&nativeMask);
                maskPainter.setRenderHint(QPainter::Antialiasing);
                maskPainter.translate(-sceneRect.topLeft());
                maskPainter.fillPath(complement, referenceMask);
            }
            painter.setRenderHint(QPainter::SmoothPixmapTransform);
            painter.drawImage(sceneRect.topLeft(), nativeMask);
        } else {
            painter.setRenderHint(QPainter::Antialiasing);
            painter.fillPath(complement, referenceMask);
        }
        painter.restore();
    }
    layer.scene()->render(&painter, sceneRect, sceneRect);
    return image;
}

QPainterPath ocrLabelScenePath(const QGraphicsItem& item) {
    QPainterPath path;
    path.addPolygon(item.mapToScene(item.boundingRect()));
    path.closeSubpath();
    return path;
}

std::optional<QPoint> blankOcrPixelInPath(const QImage& image, const QPainterPath& path) {
    const qreal dpr = image.devicePixelRatio();
    const QRectF bounds = path.boundingRect();
    const QRect pixels = QRectF(bounds.topLeft() * dpr, bounds.size() * dpr)
                             .toAlignedRect()
                             .intersected(image.rect());
    for (int y = pixels.top(); y <= pixels.bottom(); ++y) {
        for (int x = pixels.left(); x <= pixels.right(); ++x) {
            const QPointF center((x + 0.5) / dpr, (y + 0.5) / dpr);
            if (image.pixelColor(x, y) == QColor(214, 227, 239) &&
                path.contains(QRectF(center - QPointF(1, 1), QSizeF(2, 2)))) {
                return QPoint(x, y);
            }
        }
    }
    return std::nullopt;
}

QColor singleOcrMaskBlend(const QColor& mask) {
    QImage pixel(1, 1, QImage::Format_ARGB32_Premultiplied);
    pixel.fill(QColor(214, 227, 239));
    {
        QPainter painter(&pixel);
        painter.fillRect(pixel.rect(), mask);
    }
    return pixel.pixelColor(0, 0);
}

std::shared_ptr<ScreenshotOcrPresentation> ocrHighlightingPresentation() {
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(0, 0, 420, 440);
    presentation->lines = {
        ScreenshotOcrLine{QStringLiteral("A"), 1.0, ocrQuad(QRectF(20, 15, 220, 40))},
        ScreenshotOcrLine{
            QStringLiteral("Rotated"), 1.0, {{40, 85}, {260, 125}, {254, 160}, {34, 120}}},
        ScreenshotOcrLine{
            QStringLiteral("Perspective"), 1.0, {{40, 180}, {255, 165}, {245, 220}, {50, 235}}},
        ScreenshotOcrLine{QStringLiteral("ABCDE"), 1.0, ocrQuad(QRectF(320, 30, 35, 220)),
                          ScreenshotOcrTextDirection::Vertical},
        ScreenshotOcrLine{QStringLiteral("Top row\nBottom row"), 1.0,
                          ocrQuad(QRectF(40, 270, 280, 130))},
        ScreenshotOcrLine{QString{}, 1.0, ocrQuad(QRectF(350, 300, 30, 30))},
        ScreenshotOcrLine{QStringLiteral("Invalid"), 1.0, {{360, 360}, {390, 360}, {375, 390}}},
        ScreenshotOcrLine{QStringLiteral("Offscreen"), 1.0, ocrQuad(QRectF(900, 900, 50, 20))},
    };
    auto& paragraph = presentation->lines[4];
    paragraph.paragraph = true;
    paragraph.sourceLineQuads = {ocrQuad(QRectF(40, 270, 280, 30)),
                                 ocrQuad(QRectF(40, 365, 280, 35))};
    return presentation;
}

void ocrTextHighlightingMasksNonTextAroundFittedLabels() {
    ScreenshotOcrTextLayer layer;
    const auto presentation = ocrHighlightingPresentation();
    layer.setPresentation(presentation);
    layer.synchronize(QTransform(), presentation->selection);
    const auto sceneItems = layer.scene()->items();
    const auto items = ocrGlyphItems(&layer);
    require(items.size() == presentation->lines.size() &&
                std::count_if(items.cbegin(), items.cend(),
                              [](const auto* item) { return item->isVisible(); }) == 5,
            "highlighting uses only the existing valid nonempty text labels");
    require(std::any_of(items.cbegin(), items.cend(),
                        [](const auto* item) {
                            return item->isVisible() && item->sceneBoundingRect().top() < 60 &&
                                   item->sceneBoundingRect().left() < 300 &&
                                   item->sceneBoundingRect().width() < 110;
                        }),
            "highlighting fixture includes a label narrower than its recognized quad");
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    const quint64 initialSynchronizations = layer.geometrySynchronizationCount();
#endif
    for (const qreal dpr : {1.0, 1.25, 2.0}) {
        layer.setTextHighlighting(false, QColor(0, 0, 0, 128));
        const QImage disabled = renderOcrHighlightingScene(layer, dpr);
        const auto fittedItem = std::find_if(items.cbegin(), items.cend(), [](const auto* item) {
            return item->isVisible() && item->sceneBoundingRect().top() < 60 &&
                   item->sceneBoundingRect().left() < 300;
        });
        require(fittedItem != items.cend(), "the clear-text fixture contains a fitted label");
        const auto labelWhitespace = blankOcrPixelInPath(disabled, ocrLabelScenePath(**fittedItem));
        require(labelWhitespace.has_value(), "the label fixture has whitespace inside its bounds");
        layer.setTextHighlighting(true, QColor(32, 96, 192, 0));
        require(renderOcrHighlightingScene(layer, dpr) == disabled,
                "a transparent mask preserves every text and background pixel");
        for (const QColor& mask :
             {QColor(30, 120, 210, 91), QColor(180, 45, 80, 255), QColor(0, 0, 0, 128), QColor()}) {
            const QColor normalized = mask.isValid() ? mask : QColor(0, 0, 0, 128);
            layer.setTextHighlighting(false, mask);
            const QImage expected = renderOcrHighlightingScene(layer, dpr, normalized);
            layer.setTextHighlighting(true, mask);
            const QImage actual = renderOcrHighlightingScene(layer, dpr);
            if (actual != expected)
                std::cerr << "OCR mask mismatch at DPR " << dpr << " for "
                          << normalized.name(QColor::HexArgb).toStdString() << '\n';
            require(actual == expected && actual != disabled,
                    "the configured RGBA mask covers non-text areas and leaves fitted labels "
                    "and glyphs clear");
            require(actual.pixelColor(*labelWhitespace) == QColor(214, 227, 239),
                    "whitespace inside a text label remains clear instead of receiving a mask");
            for (const QPoint& blank : {QPoint(5, 5), QPoint(365, 315), QPoint(375, 375)}) {
                require(actual.pixelColor(qFloor(blank.x() * dpr), qFloor(blank.y() * dpr)) ==
                            singleOcrMaskBlend(normalized),
                        "non-text background, empty and invalid labels receive exactly one RGBA "
                        "mask blend");
            }
            require(layer.scene()->items() == sceneItems,
                    "mask toggles and color changes preserve graphics item identities");
            layer.synchronize(QTransform(), presentation->selection);
        }
        layer.setTextHighlighting(false, QColor(0, 0, 0, 128));
        require(renderOcrHighlightingScene(layer, dpr) == disabled,
                "disabling highlighting restores the original label rendering exactly");
    }
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    require(layer.geometrySynchronizationCount() == initialSynchronizations,
            "paint-only style changes must not synchronize or reshape text geometry");
#endif
    layer.setTextHighlighting(true, QColor(30, 120, 210, 91));
    const QImage highlighted = renderOcrHighlightingScene(layer, 1.25);
    layer.clearPresentation();
    require(layer.scene()->items().isEmpty(), "clearing OCR removes label items");
    layer.setPresentation(presentation);
    layer.synchronize(QTransform(), presentation->selection);
    require(renderOcrHighlightingScene(layer, 1.25) == highlighted,
            "the configured mask style survives clearing and replacing the presentation");
}

void ocrTextHighlightingUnionsOverlappingLabels() {
    ScreenshotOcrTextLayer layer;
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(0, 0, 320, 200);
    const QRectF bounds(60, 50, 200, 80);
    presentation->lines = {
        ScreenshotOcrLine{QStringLiteral("Overlap"), 1.0, ocrQuad(bounds)},
        ScreenshotOcrLine{
            QStringLiteral("Overlap"),
            1.0,
            {bounds.topLeft(), bounds.bottomLeft(), bounds.bottomRight(), bounds.topRight()}},
    };
    layer.setPresentation(presentation);
    layer.synchronize(QTransform(), presentation->selection);
    const auto sceneItems = layer.scene()->items();
    const auto items = ocrGlyphItems(&layer);
    require(items.size() == 2 && items[0]->isVisible() && items[1]->isVisible(),
            "overlapping text uses exactly two existing label items");
    const auto signedArea = [](const QPolygonF& polygon) {
        qreal area = 0;
        for (qsizetype index = 0; index < polygon.size(); ++index) {
            const QPointF& first = polygon[index];
            const QPointF& second = polygon[(index + 1) % polygon.size()];
            area += first.x() * second.y() - second.x() * first.y();
        }
        return area;
    };
    require(signedArea(items[0]->mapToScene(items[0]->boundingRect())) *
                    signedArea(items[1]->mapToScene(items[1]->boundingRect())) <
                0,
            "the overlap fixture projects text rectangles with opposite winding");
    // Keep the visible labels' exact geometry in the mask, without composing native glyph
    // antialiasing over its edge. The preceding style test checks text and mask together;
    // this fixture independently checks every pixel of the opposite-winding hole union.
    for (auto* item : items) {
        item->setOpacity(0);
        require(item->isVisible(), "transparent glyph paint must retain the label's mask hole");
    }
    const QPainterPath overlap =
        ocrLabelScenePath(*items[0]).intersected(ocrLabelScenePath(*items[1]));
    QPainterPath labelBoundaries;
    for (const auto* item : items)
        labelBoundaries.addPath(ocrLabelScenePath(*item));
    const QColor mask(30, 120, 210, 91);
    for (const qreal dpr : {1.0, 1.25, 2.0}) {
        layer.setTextHighlighting(false, mask);
        const QImage disabled = renderOcrHighlightingScene(layer, dpr);
        const auto overlapWhitespace = blankOcrPixelInPath(disabled, overlap);
        require(overlapWhitespace.has_value(), "overlapping labels have a blank interior sample");
        const QImage expected = renderOcrHighlightingScene(layer, dpr, mask);
        layer.setTextHighlighting(true, mask);
        const QImage actual = renderOcrHighlightingScene(layer, dpr);
        QPainterPathStroker boundaryStroke;
        boundaryStroke.setWidth(2.0 / dpr);
        const QPainterPath boundaryPixels = boundaryStroke.createStroke(labelBoundaries);
        const QRgb clear = QColor(214, 227, 239).rgba();
        const QRgb fullyMasked = singleOcrMaskBlend(mask).rgba();
        bool masksMatch = actual.size() == expected.size() &&
                          actual.devicePixelRatio() == expected.devicePixelRatio() &&
                          actual.format() == expected.format();
        for (int y = 0; y < actual.height() && masksMatch; ++y) {
            for (int x = 0; x < actual.width(); ++x) {
                const QRgb observed = actual.pixel(x, y);
                const QRgb reference = expected.pixel(x, y);
                if (observed == reference)
                    continue;
                // Independently subtracting the same floating-point contours can round an
                // antialiased boundary channel by one. Clear/fully covered pixels remain exact;
                // only proven, partially covered boundary pixels admit that quantization.
                masksMatch = reference != clear && reference != fullyMasked && observed != clear &&
                             observed != fullyMasked &&
                             boundaryPixels.contains(QPointF((x + 0.5) / dpr, (y + 0.5) / dpr)) &&
                             qAlpha(observed) == qAlpha(reference) &&
                             qAbs(qRed(observed) - qRed(reference)) <= 1 &&
                             qAbs(qGreen(observed) - qGreen(reference)) <= 1 &&
                             qAbs(qBlue(observed) - qBlue(reference)) <= 1;
                if (!masksMatch)
                    break;
            }
        }
        require(masksMatch && actual.pixelColor(*overlapWhitespace) == QColor(214, 227, 239),
                "overlapping labels form a union of clear holes regardless of winding");
        require(actual.pixelColor(qFloor(5 * dpr), qFloor(5 * dpr)) == singleOcrMaskBlend(mask) &&
                    layer.scene()->items() == sceneItems,
                "overlapping labels never multiply background opacity or create overlay items");
    }
}

void ocrTextHighlightingTracksFractionalDevicePhase() {
    const auto presentation = ocrHighlightingPresentation();
    presentation->selection = QRect(0, 0, 640, 640);
    // Dense contours exercise raster reuse; simple masks use the direct vector fill.
    for (int index = 0; index < 48; ++index) {
        presentation->lines.push_back(ScreenshotOcrLine{
            QStringLiteral("Z"), 1.0,
            ocrQuad(QRectF(30 + (index % 8) * 70, 460 + (index / 8) * 28, 50, 20))});
    }
    ScreenshotOcrTextLayer layer;
    ScreenshotOcrTextLayer reference;
    layer.setPresentation(presentation);
    reference.setPresentation(presentation);
    layer.synchronize(QTransform(), presentation->selection);
    reference.synchronize(QTransform(), presentation->selection);
    const auto sceneItems = layer.scene()->items();
    const auto items = ocrGlyphItems(&layer);
    const QColor mask(30, 120, 210, 91);
    layer.setTextHighlighting(true, mask);
    constexpr qreal dpr = 1.25;
    const QImage identityExpected = renderOcrHighlightingScene(reference, dpr, mask);
    const QImage identityActual = renderOcrHighlightingScene(layer, dpr);
    const QTransform initialPhase = QTransform::fromTranslate(0.25, 0.4);
    const QImage initial = renderOcrHighlightingScene(layer, dpr, {}, initialPhase);
    for (const QTransform& transform :
         {initialPhase, QTransform::fromTranslate(0.6, 0.1), initialPhase,
          QTransform(1.1, 0, 0, 0.9, 0.3, 0.2), initialPhase}) {
        const QImage expected = renderOcrHighlightingScene(reference, dpr, mask, transform);
        const QImage actual = renderOcrHighlightingScene(layer, dpr, {}, transform);
        const bool metadataEqual = actual.format() == expected.format() &&
                                   actual.devicePixelRatio() == expected.devicePixelRatio() &&
                                   actual.colorSpace() == expected.colorSpace();
        require(metadataEqual, "cached and reference masks have identical image metadata");
        require(actual == expected,
                "mask cache follows fractional device phase and nonuniform transforms exactly");
        if (transform == initialPhase) {
            require(actual == initial,
                    "returning to the initial device phase restores its exact mask pixels");
        }
        require(layer.scene()->items() == sceneItems,
                "device-phase mask updates preserve the existing text-label items");
    }
    require(identityActual == identityExpected,
            "cached and reference masks agree at the identity device phase");
}

void ocrTextHighlightingPreservesHiddenTextSelection() {
    ScreenshotOcrTextLayer layer;
    const auto presentation = ocrHighlightingPresentation();
    layer.setTextHighlighting(true, QColor(180, 45, 80, 255));
    layer.setPresentation(presentation, ScreenshotOcrTextLayer::RenderingMode::SelectionOnly);
    layer.synchronize(QTransform(), presentation->selection);
    const auto sceneItems = layer.scene()->items();
    const auto items = ocrGlyphItems(&layer);
    for (const qreal dpr : {1.0, 1.25, 2.0}) {
        presentation->clearTextSelection();
        layer.updateSelection();
        layer.setTextHighlighting(false, QColor(180, 45, 80, 255));
        const QImage hidden = renderOcrHighlightingScene(layer, dpr);
        layer.setTextHighlighting(true, QColor(180, 45, 80, 255));
        require(renderOcrHighlightingScene(layer, dpr) == hidden,
                "hidden selectable OCR must never paint text highlighting");
        presentation->selectAll();
        layer.updateSelection();
        const QImage selected = renderOcrHighlightingScene(layer, dpr);
        layer.setTextHighlighting(false, QColor(180, 45, 80, 255));
        require(renderOcrHighlightingScene(layer, dpr) == selected && selected != hidden,
                "SelectionOnly preserves character selection while suppressing label masks");
        require(layer.scene()->items() == sceneItems,
                "hidden label style changes preserve the existing scene items");
    }
}

void ocrTextHighlightingPreservesInteractionAndExports() {
    QWidget host;
    host.resize(320, 180);
    QPalette hostPalette = host.palette();
    hostPalette.setColor(QPalette::Window, QColor(214, 227, 239));
    host.setPalette(hostPalette);
    host.setAutoFillBackground(true);
    host.show();
    ScreenshotRecognitionWindow window(
        {}, &host, ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
    require(window.present({QGuiApplication::primaryScreen(), &host, host.rect(), host.rect(),
                            ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild}),
            "the highlighting recognition surface presents");
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = host.rect();
    presentation->lines = {
        ScreenshotOcrLine{QStringLiteral("Mask sample"), 1.0, ocrQuad(QRectF(35, 40, 220, 35))},
        ScreenshotOcrLine{QStringLiteral("Second line"), 1.0, ocrQuad(QRectF(35, 105, 220, 35))},
    };
    window.setOcrPresentation(presentation);
    window.setOcrCopyDefaultsEnabled(false);
    QApplication::processEvents();
    auto* layer = static_cast<ScreenshotOcrTextLayer*>(
        window.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer")));
    require(layer != nullptr, "the window owns the OCR label layer");
    const auto sceneItems = layer->scene()->items();
    const auto items = ocrGlyphItems(layer);
    const QPointF hitPoint(80, 55);
    const auto position = layer->textPositionAt(hitPoint, false);
    require(position.valid() && !window.isOcrBackgroundAt(hitPoint) &&
                window.isOcrBackgroundAt(QPointF(10, 10)),
            "the highlighting fixture has distinct text and background hit regions");
    QImage source(host.size(), QImage::Format_ARGB32_Premultiplied);
    source.fill(QColor(22, 90, 130));
    const auto originalSnapshot = window.imageSnapshot(source, host.rect(), {}, {}, {});
    require(originalSnapshot.has_value(), "OCR exports produce an image snapshot");
    const QImage originalExport = renderScreenshotRecognitionImage(*originalSnapshot);
    const QImage originalPrint = window.printViewportSnapshot(source, host.rect());
    require(!originalExport.isNull() && !originalPrint.isNull(),
            "highlighting export references contain rendered text");
    const QImage defaultPixels = renderOcrHighlightingScene(*layer, 1.0);
    window.setTextHighlighting(false, QColor(0, 0, 0, 128));
    require(renderOcrHighlightingScene(*layer, 1.0) == defaultPixels,
            "text highlighting is disabled on a newly created recognition window");
    const QColor mask(30, 120, 210, 91);
    QApplication::processEvents();
    const QImage originalHost = host.grab().toImage();
    window.setTextHighlighting(true, mask);
    QApplication::processEvents();
    require(host.grab().toImage() != originalHost,
            "enabling highlighting repaints the composed parent surface");
    window.setTextHighlighting(false, mask);
    QApplication::processEvents();
    require(host.grab().toImage() == originalHost,
            "disabling highlighting clears its backing pixels and restores the parent surface");
    presentation->beginTextSelection(ScreenshotOcrTextPosition{0, 0});
    presentation->updateTextSelection(ScreenshotOcrTextPosition{0, 2});
    presentation->finishTextSelection();
    window.updateOcrSelection();
    const auto originalCopy = window.textSelectionClipboardMimeData();
    require(originalCopy && originalCopy->text() == QStringLiteral("Ma"),
            "highlighting selection fixture copies its exact selected text");
    const QImage expectedSelection = renderOcrHighlightingScene(*layer, 1.25, mask, {}, true);
    window.setTextHighlighting(true, mask);
    require(renderOcrHighlightingScene(*layer, 1.25) == expectedSelection,
            "character selection and glyphs remain unchanged inside clear label regions");
    const auto highlightedPosition = layer->textPositionAt(hitPoint, false);
    const auto highlightedCopy = window.textSelectionClipboardMimeData();
    require(highlightedPosition.lineIndex == position.lineIndex &&
                highlightedPosition.characterIndex == position.characterIndex &&
                !window.isOcrBackgroundAt(hitPoint) && window.isOcrBackgroundAt(QPointF(10, 10)) &&
                highlightedCopy && highlightedCopy->text() == originalCopy->text() &&
                layer->scene()->items() == sceneItems,
            "highlighting preserves text hit tests, clipboard data and graphics item identities");
    const auto highlightedSnapshot = window.imageSnapshot(source, host.rect(), {}, {}, {});
    require(highlightedSnapshot &&
                renderScreenshotRecognitionImage(*highlightedSnapshot) == originalExport &&
                window.printViewportSnapshot(source, host.rect()) == originalPrint,
            "screen highlighting never changes recognized image exports or printed pixels");
    const auto changedItem = std::find_if(items.cbegin(), items.cend(), [](const auto* item) {
        return item->sceneBoundingRect().center().y() > 100;
    });
    require(changedItem != items.cend(), "the incremental fixture has a second text label");
    const QRectF originalLabelBounds = (*changedItem)->sceneBoundingRect();
    const QPainterPath originalHole = ocrLabelScenePath(**changedItem);
    const QImage beforeUpdate = renderOcrHighlightingScene(*layer, 1.25);
    window.updateOcrText(1, QStringLiteral("B"));
    const QImage updated = renderOcrHighlightingScene(*layer, 1.25);
    const QPainterPath vacatedHole = originalHole.subtracted(ocrLabelScenePath(**changedItem));
    const auto vacatedWhitespace = blankOcrPixelInPath(beforeUpdate, vacatedHole);
    require(vacatedWhitespace.has_value() &&
                updated.pixelColor(*vacatedWhitespace) == singleOcrMaskBlend(mask),
            "shrinking a text label remasks its former clear area exactly once");
    require(layer->scene()->items() == sceneItems &&
                (*changedItem)->sceneBoundingRect().width() < originalLabelBounds.width() / 2 &&
                presentation->selectedText() == QStringLiteral("Ma"),
            "incremental OCR text changes resize the highlighted label without replacing items "
            "or disturbing another line's selection");
    const QRect unchangedPixels(0, 0, updated.width(), qRound(90 * 1.25));
    require(updated.copy(unchangedPixels) == beforeUpdate.copy(unchangedPixels),
            "incremental highlighted text updates preserve every pixel of the other label");
    window.setTextHighlighting(false, mask);
    // Embedded OCR scales its native background even when painting to a different DPR.
    const QImage expectedUpdate = renderOcrHighlightingScene(*layer, 1.25, mask, {}, true);
    window.setTextHighlighting(true, mask);
    require(updated == expectedUpdate && renderOcrHighlightingScene(*layer, 1.25) == expectedUpdate,
            "the clear hole follows updated fitted bounds without retaining the old rectangle");
    const QImage highlighted = renderOcrHighlightingScene(*layer, 1.25);
    window.clearOcrPresentation();
    window.setOcrPresentation(presentation);
    require(renderOcrHighlightingScene(*layer, 1.25) == highlighted,
            "window presentation replacement preserves its configured highlighting style");
}

void ocrTextHighlightingPreservesBoxSelection() {
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(0, 0, 320, 220);
    for (int index = 0; index < 3; ++index) {
        presentation->lines.push_back(
            ScreenshotOcrLine{QStringLiteral("block %1").arg(index), 1.0,
                              ocrQuad(QRectF(40, 30 + index * 60, 220, 35))});
    }
    ScreenshotOcrTextLayer layer;
    ScreenshotOcrTextLayer reference;
    layer.setPresentation(presentation);
    reference.setPresentation(presentation);
    layer.synchronize(QTransform(), presentation->selection);
    reference.synchronize(QTransform(), presentation->selection);
    const auto sceneItems = layer.scene()->items();
    const QColor mask(30, 120, 210, 91);
    layer.setTextHighlighting(true, mask);
    const auto verifyPixels = [&] {
        layer.updateSelection();
        reference.updateSelection();
        for (const qreal dpr : {1.0, 1.25, 2.0}) {
            require(renderOcrHighlightingScene(layer, dpr) ==
                        renderOcrHighlightingScene(reference, dpr, mask),
                    "highlighting masks only non-text areas through box preview transitions");
        }
        require(layer.scene()->items() == sceneItems,
                "box selection and highlighting retain every glyph and overlay item");
    };
    presentation->beginTextSelection(ScreenshotOcrTextPosition{0, 0});
    presentation->updateTextSelection(
        ScreenshotOcrTextPosition{0, static_cast<int>(presentation->lines[0].text.size())});
    presentation->finishTextSelection();
    presentation->beginLineBoxSelection(QPointF(10, 10));
    presentation->updateLineBoxSelection(QRectF(10, 10, 280, 120));
    require(presentation->lineSelectionPreview(0) == ScreenshotOcrLineSelectionPreview::Deselect &&
                presentation->lineSelectionPreview(1) == ScreenshotOcrLineSelectionPreview::Select,
            "highlighting fixture exercises simultaneous selection and deselection previews");
    verifyPixels();
    layer.setTextHighlighting(false, mask);
    require(presentation->lineBoxSelectionActive() &&
                presentation->selectedText() == QStringLiteral("block 0"),
            "a highlighting toggle preserves the active gesture and committed selection");
    layer.setTextHighlighting(true, mask);
    presentation->updateLineBoxSelection(QRectF(10, 10, 280, 60));
    verifyPixels();
    presentation->cancelLineBoxSelection();
    verifyPixels();
    require(presentation->selectedText() == QStringLiteral("block 0"),
            "canceling a highlighted box gesture preserves its committed selection");
    presentation->beginLineBoxSelection(QPointF(10, 10));
    presentation->updateLineBoxSelection(QRectF(10, 10, 280, 120));
    presentation->finishLineBoxSelection();
    verifyPixels();
    require(presentation->selectedText() == QStringLiteral("block 1"),
            "committing a highlighted box gesture toggles only its candidate blocks");
}

void ocrTextHighlightingTracksFontChanges() {
    const QFont originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([&] { QApplication::setFont(originalFont); });
    ScreenshotOcrTextLayer layer;
    const auto presentation = ocrHighlightingPresentation();
    layer.setPresentation(presentation);
    layer.synchronize(QTransform(), presentation->selection);
    const auto sceneItems = layer.scene()->items();
    const auto items = ocrGlyphItems(&layer);
    QList<QRectF> initialBounds;
    for (const auto* item : items)
        initialBounds.push_back(item->sceneBoundingRect());
    const QColor mask(30, 120, 210, 91);
    layer.setTextHighlighting(true, mask);
    QFont replacement = originalFont;
    replacement.setStretch(QFont::Expanded);
    QApplication::setFont(replacement);
    QApplication::processEvents();
    bool geometryChanged = false;
    for (qsizetype index = 0; index < items.size(); ++index)
        geometryChanged |= items[index]->sceneBoundingRect() != initialBounds[index];
    require(geometryChanged && layer.scene()->items() == sceneItems,
            "application font changes reshape existing fitted OCR labels");
    const QImage highlighted = renderOcrHighlightingScene(layer, 1.25);
    layer.setTextHighlighting(false, mask);
    require(highlighted == renderOcrHighlightingScene(layer, 1.25, mask),
            "font changes refresh the mask holes together with the glyph cache");
}

void ocrImageHighlightingReusesNativeBackgroundAcrossZooms() {
    auto presentation = ocrHighlightingPresentation();
    const QPoint origin(70, -30);
    presentation->selection.translate(origin);
    for (auto& line : presentation->lines) {
        line.quad.translate(origin);
        for (auto& quad : line.sourceLineQuads)
            quad.translate(origin);
    }
    const QColor mask(30, 120, 210, 91);
    ScreenshotOcrTextLayer reference;
    reference.setPresentation(presentation);
    reference.synchronize(QTransform::fromTranslate(-origin.x(), -origin.y()),
                          QRect(QPoint(), presentation->selection.size()));
    QPainterPath complement;
    complement.addRect(reference.sceneRect());
    for (const auto* item : ocrGlyphItems(&reference)) {
        if (item->isVisible())
            complement = complement.subtracted(ocrLabelScenePath(*item));
    }
    QImage nativeBackground(presentation->selection.size(), QImage::Format_ARGB32_Premultiplied);
    nativeBackground.fill(Qt::transparent);
    {
        QPainter painter(&nativeBackground);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.fillPath(complement, mask);
    }

    ScreenshotOcrTextLayer layer(nullptr, ScreenshotOcrTextLayer::HighlightingResolution::Image);
    layer.setPresentation(presentation);
    layer.setTextHighlighting(true, mask);
    const auto items = ocrGlyphItems(&layer);
    for (auto* item : items)
        item->setOpacity(0); // Inspect the background independently of current-zoom glyphs.
    qint64 cacheKey = 0;
    for (const qreal zoom : {0.5, 1.0, 1.7, 0.75, 2.0, 0.5}) {
        for (const qreal dpr : {1.0, 1.25, 2.0}) {
            // The initial paint is zoomed out. Later pans expose labels that were culled.
            const QPointF pan = zoom > 1 ? QPointF(-105.25, -215.4) : QPointF(-0.25, -0.4);
            const QTransform nativeToView(zoom, 0, 0, zoom, pan.x(), pan.y());
            const QTransform canvasToView =
                QTransform::fromTranslate(-origin.x(), -origin.y()) * nativeToView;
            layer.synchronize(canvasToView,
                              QRect(0, 0, 160 + qRound(zoom * 10), 170 + qRound(zoom * 10)));
            const QImage actual = renderOcrHighlightingScene(layer, dpr);
            QImage expected(actual.size(), actual.format());
            expected.setDevicePixelRatio(dpr);
            expected.fill(QColor(214, 227, 239));
            {
                QPainter painter(&expected);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.setClipRect(layer.sceneRect());
                painter.setTransform(nativeToView);
                painter.drawImage(QPointF(), nativeBackground);
            }
            if (actual != expected)
                std::cerr << "Native OCR background mismatch at zoom " << zoom << " DPR " << dpr
                          << '\n';
            require(actual == expected,
                    "zoomed OCR background must scale the native mask, including offscreen labels");
            const QImage cached = layer.highlightingRasterForTesting();
            require(cached.size() == nativeBackground.size() && cached.devicePixelRatio() == 1.0,
                    "first paint at any zoom caches exactly one pixel per source-image pixel");
            if (cacheKey == 0)
                cacheKey = cached.cacheKey();
            require(
                cached.cacheKey() == cacheKey && layer.highlightingRasterBuildCount() == 1,
                "zoom, pan, resize, DPR and repeat paints must reuse one native background raster");
        }
    }
    layer.synchronize(QTransform::fromTranslate(40 - origin.x(), 50 - origin.y()),
                      QRect(0, 0, 300, 260));
    require(renderOcrHighlightingScene(layer, 1.0).pixelColor(5, 5) == singleOcrMaskBlend(mask) &&
                layer.highlightingRasterForTesting().cacheKey() == cacheKey,
            "panning outside the source masks the exposed exterior without rebuilding the cache");
    presentation->selectAll();
    layer.updateSelection();
    static_cast<void>(renderOcrHighlightingScene(layer, 1.0));
    require(layer.highlightingRasterBuildCount() == 1,
            "text selection must not invalidate the cached surrounding background");
    layer.setTextHighlighting(true, QColor(180, 45, 80, 255));
    static_cast<void>(renderOcrHighlightingScene(layer, 1.0));
    require(layer.highlightingRasterBuildCount() == 2,
            "changing the mask color rebuilds the native background once");
    presentation->setLineText(0, QStringLiteral("Replacement text"));
    layer.updateLineText(0);
    static_cast<void>(renderOcrHighlightingScene(layer, 1.0));
    require(layer.highlightingRasterBuildCount() == 3,
            "editing OCR text refreshes its cached fitted hole");
    const QFont originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([&] { QApplication::setFont(originalFont); });
    QFont font = originalFont;
    font.setStretch(QFont::Expanded);
    QApplication::setFont(font);
    QApplication::processEvents();
    static_cast<void>(renderOcrHighlightingScene(layer, 1.0));
    require(layer.highlightingRasterBuildCount() == 4,
            "font changes refresh cached background geometry");
    layer.setTextHighlighting(false, mask);
    require(layer.highlightingRasterForTesting().isNull(),
            "disabling highlighting releases the native background image");
    layer.setTextHighlighting(true, mask);
    static_cast<void>(renderOcrHighlightingScene(layer, 1.0));
    require(layer.highlightingRasterBuildCount() == 5,
            "reenabling highlighting rebuilds the native background once");
    layer.clearPresentation();
    require(layer.highlightingRasterForTesting().isNull(),
            "ending recognition releases the cached background");
    layer.setPresentation(presentation, ScreenshotOcrTextLayer::RenderingMode::SelectionOnly);
    layer.synchronize(QTransform(), QRect(0, 0, 160, 170));
    static_cast<void>(renderOcrHighlightingScene(layer, 1.0));
    require(layer.highlightingRasterForTesting().isNull(),
            "hidden text selection must never allocate a highlighting background");

    // A large source must still be cached when zoomed out; the old viewport-raster
    // byte limit must not turn every subsequent zoom into another vector background paint.
    auto large = std::make_shared<ScreenshotOcrPresentation>();
    large->selection = QRect(0, 0, 4200, 4100);
    large->lines = {
        ScreenshotOcrLine{QStringLiteral("Large image"), 1.0, ocrQuad(QRectF(100, 100, 200, 40))}};
    layer.setPresentation(large);
    layer.synchronize(QTransform::fromScale(0.01, 0.01), QRect(0, 0, 42, 41));
    static_cast<void>(renderOcrHighlightingScene(layer, 1.0));
    require(layer.highlightingRasterForTesting().size() == large->selection.size(),
            "replacing the source builds a full native background even for large images");
    const auto largeKey = layer.highlightingRasterForTesting().cacheKey();
    layer.synchronize(QTransform::fromScale(0.02, 0.02), QRect(0, 0, 84, 82));
    static_cast<void>(renderOcrHighlightingScene(layer, 1.0));
    require(layer.highlightingRasterForTesting().cacheKey() == largeKey,
            "large-image zooms reuse the native background");
}

void ocrTextHighlightingTests() {
    const QFont originalFont = QApplication::font();
    const auto restoreFont = qScopeGuard([&]() { QApplication::setFont(originalFont); });
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >=
                    0,
                "load offscreen text highlighting font");
        QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
    }
#endif
    ocrTextHighlightingMasksNonTextAroundFittedLabels();
    ocrTextHighlightingUnionsOverlappingLabels();
    ocrTextHighlightingTracksFractionalDevicePhase();
    ocrTextHighlightingPreservesHiddenTextSelection();
    ocrTextHighlightingPreservesInteractionAndExports();
    ocrTextHighlightingPreservesBoxSelection();
    ocrTextHighlightingTracksFontChanges();
    ocrImageHighlightingReusesNativeBackgroundAcrossZooms();
}

void selectionOnlyTextLayerPaintsOnlyHighlights() {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >=
                    0,
                "load offscreen recognition font");
        QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
    }
#endif
    QWidget host;
    host.resize(260, 180);
    QPalette palette = host.palette();
    palette.setColor(QPalette::Window, QColor(24, 72, 120));
    host.setPalette(palette);
    host.setAutoFillBackground(true);
    host.show();
    const QImage original = host.grab().toImage();
    ScreenshotRecognitionWindow recognition(
        {}, &host, ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
    require(recognition.present({QGuiApplication::primaryScreen(), &host, host.rect(), host.rect(),
                                 ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild}),
            "selection-only embedded presentation initializes");
    for (int kind = 0; kind != 3; ++kind) {
        auto presentation = std::make_shared<ScreenshotOcrPresentation>();
        presentation->selection = host.rect();
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Selectable text");
        // Cached OCR can retain fill colors from an earlier displayed presentation.
        // That metadata must never make text visible in selection-only mode.
        line.backgroundFillColor = kind == 1 ? QColor(Qt::white) : QColor(Qt::black);
        const QRectF bounds = kind == 1 ? QRectF(80, 10, 24, 155) : QRectF(20, 40, 220, 60);
        line.quad = QPolygonF(
            {bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()});
        if (kind == 1) {
            line.direction = ScreenshotOcrTextDirection::Vertical;
        } else if (kind == 2) {
            line.paragraph = true;
            line.sourceLineQuads = {line.quad};
        }
        presentation->lines = {line};
        presentation->prepareForRendering();
        recognition.setOcrPresentation(presentation,
                                       ScreenshotOcrTextLayer::RenderingMode::SelectionOnly, false);
        recognition.show();
        QApplication::processEvents();
        require(host.grab().toImage() == original,
                "hidden text and regions must not change any image pixel");
        presentation->selectAll();
        recognition.updateOcrSelection();
        QApplication::processEvents();
        const QImage selected = host.grab().toImage();
        require(selected != original, "hidden selection must render its highlight");
        auto* layer = recognition.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
        require(layer != nullptr, "selection-only text layer exists");
        QImage overlay(host.size(), QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);
        {
            QPainter painter(&overlay);
            layer->scene()->render(&painter, QRectF(overlay.rect()), layer->sceneRect());
        }
        int maximumAlpha = 0;
        for (int y = 0; y < overlay.height(); ++y) {
            for (int x = 0; x < overlay.width(); ++x) {
                maximumAlpha = std::max(maximumAlpha, qAlpha(overlay.pixel(x, y)));
            }
        }
        require(maximumAlpha == 102,
                "hidden selection paints only a 40-percent highlight, with no opaque glyphs");
        const qreal dpr = selected.devicePixelRatio();
        const QRect allowed = QRectF(bounds.topLeft() * dpr, bounds.size() * dpr)
                                  .toAlignedRect()
                                  .adjusted(-1, -1, 1, 1);
        for (int y = 0; y < selected.height(); ++y) {
            for (int x = 0; x < selected.width(); ++x) {
                require(selected.pixel(x, y) == original.pixel(x, y) || allowed.contains(x, y),
                        "selection paints only within the recognized text region");
            }
        }
        recognition.clearOcrSelection();
        QApplication::processEvents();
        require(host.grab().toImage() == original,
                "clearing selection restores exact image pixels");
        recognition.setOcrPresentation(presentation);
        QApplication::processEvents();
        require(host.grab().toImage() != original, "normal mode still paints recognized text");
    }
}

void embeddedRecognitionWindowPreservesParentSurfaceWithVisibleTextLayer() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    const QColor background(24, 72, 120, 255);
    QWidget host;
    host.resize(160, 90);
    QPalette palette = host.palette();
    palette.setColor(QPalette::Window, background);
    host.setPalette(palette);
    host.setAutoFillBackground(true);
    host.show();

    ScreenshotRecognitionWindow recognition(
        ScreenshotRecognitionWindowActions{}, &host,
        ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
    require(recognition.present(ScreenshotRecognitionWindow::Config{
                screen,
                &host,
                host.rect(),
                QRectF(QPointF(), QSizeF(host.size())),
                ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild,
            }),
            "an embedded recognition window should accept its parent geometry");
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = host.rect();
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Embedded OCR");
    line.quad = QPolygonF(
        {QPointF(45.0, 35.0), QPointF(115.0, 35.0), QPointF(115.0, 55.0), QPointF(45.0, 55.0)});
    presentation->lines.push_back(line);
    ScreenshotOcrLine secondLine;
    secondLine.text = QStringLiteral("stable");
    secondLine.quad =
        QPolygonF({QPointF(120, 65), QPointF(150, 65), QPointF(150, 80), QPointF(120, 80)});
    presentation->lines.push_back(secondLine);
    presentation->prepareForRendering();
    recognition.setOcrPresentation(presentation);
    QApplication::processEvents();
    auto* textLayer = recognition.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
    require(textLayer != nullptr && textLayer->isVisible(),
            "the embedded recognition text layer should participate in composition");
    require(recognition.isOcrBackgroundAt(QPointF(10, 10)) &&
                !recognition.isOcrBackgroundAt(QPointF(80, 45)),
            "OCR hit testing must distinguish text from empty background");
    const QPoint textPoint(80, 45);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(textPoint),
                      QPointF(recognition.mapToGlobal(textPoint)), Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&recognition, &press);
    require(presentation->textSelectionActive() && !recognition.isOcrBackgroundAt(QPointF(10, 10)),
            "dragging a text selection outside its line must not become a window move");
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(textPoint),
                        QPointF(recognition.mapToGlobal(textPoint)), Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&recognition, &release);
    require(!presentation->textSelectionActive() && recognition.isOcrBackgroundAt(QPointF(10, 10)),
            "OCR background dragging must become available after selection ends");

    QImage rendered(host.size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    QPainter painter(&rendered);
    host.render(&painter, QPoint(), QRegion(), QWidget::DrawChildren);
    painter.end();

    require(rendered.pixelColor(QPoint(10, 10)) == background,
            "an embedded recognition surface must not replace its parent's painted pixels");
    const auto items = textLayer->scene()->items();
    presentation->beginTextSelection(ScreenshotOcrTextPosition{1, 0});
    presentation->updateTextSelection(ScreenshotOcrTextPosition{1, 3});
    presentation->finishTextSelection();
    recognition.updateOcrText(0, QString::fromUcs4(U"\u7ffb\u8bd1\u6587\u5b57\U0001f642"));
    QApplication::processEvents();
    require(textLayer->scene()->items() == items &&
                presentation->selectedText() == QStringLiteral("sta"),
            "incremental translation should preserve graphics items and unrelated selection");
    QImage translated(host.size(), QImage::Format_ARGB32_Premultiplied);
    translated.fill(Qt::transparent);
    QPainter translatedPainter(&translated);
    host.render(&translatedPainter, QPoint(), QRegion(), QWidget::DrawChildren);
    translatedPainter.end();
    require(translated.pixelColor(QPoint(10, 10)) == background &&
                translated.copy(QRect(45, 35, 70, 20)) != rendered.copy(QRect(45, 35, 70, 20)),
            "streamed text should repaint its box while retaining the parent background");

    QTextDocument editorDocument(QStringLiteral("editable"));
    recognition.showTextEditor(&editorDocument);
    require(!recognition.isOcrBackgroundAt(QPointF(10, 10)),
            "text-editor panels must retain their mouse input");
    recognition.hideTextEditor();
}

void ocrHoverUpdatesCursorWithoutClicking() {
    using Mode = ScreenshotRecognitionWindow::PresentationMode;
    for (const auto mode : {Mode::TopLevelWindow, Mode::EmbeddedChild}) {
        QWidget host;
        host.resize(240, 160);
        host.show();
        ScreenshotRecognitionWindow recognition(ScreenshotRecognitionWindowActions{}, &host, mode);
        require(recognition.present({QGuiApplication::primaryScreen(), &host, host.rect(),
                                     QRectF(host.rect()), mode}),
                "OCR hover fixture should present in both window modes");
        auto presentation = std::make_shared<ScreenshotOcrPresentation>();
        presentation->selection = host.rect();
        ScreenshotOcrLine line;
        line.text = QStringLiteral("Hover text");
        line.quad = QPolygonF(QRectF(40, 30, 140, 30));
        presentation->lines.push_back(line);
        presentation->prepareForRendering();
        recognition.setOcrPresentation(presentation);
        QApplication::processEvents();

        const auto hover = [&](const QPoint& point, Qt::CursorShape expected) {
            // Use the actual hit-test receiver: sending directly to the recognition
            // window bypasses the content container's mouse-tracking policy.
            QWidget* receiver = recognition.childAt(point);
            require(receiver != nullptr, "OCR content should have a mouse event receiver");
            QMouseEvent move(QEvent::MouseMove, QPointF(receiver->mapFrom(&recognition, point)),
                             QPointF(recognition.mapToGlobal(point)), Qt::NoButton, Qt::NoButton,
                             Qt::NoModifier);
            QApplication::sendEvent(receiver, &move);
            require(receiver->cursor().shape() == expected,
                    "OCR hover must update the cursor without a mouse button held");
            require(!presentation->textSelectionActive(), "hover must not begin a text selection");
        };
        hover(QPoint(100, 45), Qt::IBeamCursor);
        hover(QPoint(10, 100), Qt::ArrowCursor);
        hover(QPoint(100, 45), Qt::IBeamCursor);
    }
}

void ocrBoxSelectionUsesCurrentRectangleAndCommitsOnRelease() {
    using Mode = ScreenshotRecognitionWindow::PresentationMode;
    for (const auto mode : {Mode::TopLevelWindow, Mode::EmbeddedChild}) {
        QWidget host;
        host.resize(320, 200);
        host.show();
        ScreenshotRecognitionWindow recognition({}, &host, mode);
        const QRectF canvas(100, 80, 640, 400);
        require(recognition.present(
                    {QGuiApplication::primaryScreen(), &host, host.rect(), canvas, mode}),
                "box selection fixture presents in both window modes");
        auto presentation = std::make_shared<ScreenshotOcrPresentation>();
        presentation->selection = canvas.toRect();
        for (int index = 0; index < 3; ++index) {
            ScreenshotOcrLine line;
            line.text = QStringLiteral("block %1").arg(index);
            line.quad = ocrQuad(QRectF(180, 140 + index * 100, 360, 60));
            presentation->lines.push_back(line);
        }
        recognition.setOcrPresentation(presentation);
        recognition.setOcrCopyDefaultsEnabled(false);
        recognition.setTextHighlighting(true, QColor(30, 120, 210, 91));
        QApplication::processEvents();
        const auto send = [&](QEvent::Type type, const QPointF& local,
                              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
            QWidget* receiver = type == QEvent::MouseButtonPress
                                    ? recognition.childAt(local.toPoint())
                                    : &recognition;
            require(receiver != nullptr, "blank OCR press has a real child event receiver");
            const QPointF receiverPosition =
                receiver == &recognition ? local
                                         : local - QPointF(receiver->mapTo(&recognition, QPoint()));
            QMouseEvent event(
                type, receiverPosition, QPointF(recognition.mapToGlobal(local.toPoint())),
                type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
            QApplication::sendEvent(receiver, &event);
        };
        const auto selectBlock = [&](int index) {
            const QPointF start(10, 15 + index * 50);
            const QPointF end(240, 65 + index * 50);
            send(QEvent::MouseButtonPress, start);
            send(QEvent::MouseMove, end);
            send(QEvent::MouseButtonRelease, end);
        };
        selectBlock(0);
        selectBlock(2);
        require(presentation->selectedText() == QStringLiteral("block 0\nblock 2"),
                "separate rectangles retain independent blocks in reading order");
        send(QEvent::MouseButtonPress, QPointF(10, 10));
        require(recognition.lineBoxSelectionActive() &&
                    presentation->selectedText() == QStringLiteral("block 0\nblock 2"),
                "blank press preserves committed selections until release");
        send(QEvent::MouseMove, QPointF(12, 12));
        require(presentation->lineBoxSelectionRect().isEmpty(),
                "sub-threshold motion does not paint a box");
        send(QEvent::MouseMove, QPointF(240, 180));
        require(recognition.cursor() ==
                    snow_shot::presentation::crosshairCursor(recognition.devicePixelRatioF()),
                "OCR box selection must use Snow Shot's crosshair");
        require(presentation->lineSelectionPreview(0) ==
                        ScreenshotOcrLineSelectionPreview::Deselect &&
                    presentation->lineSelectionPreview(1) ==
                        ScreenshotOcrLineSelectionPreview::Select &&
                    presentation->lineSelectionPreview(2) ==
                        ScreenshotOcrLineSelectionPreview::Deselect &&
                    presentation->selectedText() == QStringLiteral("block 0\nblock 2"),
                "mixed green/red previews retain committed clipboard contents");
        send(QEvent::MouseMove, QPointF(240, 70), Qt::AltModifier);
        require(
            presentation->lineSelectionPreview(1) == ScreenshotOcrLineSelectionPreview::None &&
                presentation->lineSelectionPreview(2) == ScreenshotOcrLineSelectionPreview::None,
            "shrinking the current rectangle removes excluded previews despite modifier changes");
        send(QEvent::MouseButtonRelease, QPointF(240, 70), Qt::AltModifier);
        require(!recognition.lineBoxSelectionActive() &&
                    presentation->selectedText() == QStringLiteral("block 2") &&
                    QWidget::mouseGrabber() != &recognition,
                "release applies only the final candidates and relinquishes capture");
        require(recognition.copyVisibleContentToClipboard() &&
                    QApplication::clipboard()->text() == QStringLiteral("block 2"),
                "clipboard uses committed separate-block selection");
        send(QEvent::MouseButtonPress, QPointF(10, 10));
        send(QEvent::MouseButtonRelease, QPointF(10, 10));
        require(!presentation->hasTextSelection(), "a plain blank click still clears selection");
    }
}

void ocrBoxSelectionCancelsWithoutTriggeringWindowCommands() {
    using Mode = ScreenshotRecognitionWindow::PresentationMode;
    for (const auto mode : {Mode::TopLevelWindow, Mode::EmbeddedChild}) {
        QWidget host;
        host.resize(240, 120);
        host.show();
        int cancelCommands = 0;
        ScreenshotRecognitionWindowActions actions;
        actions.handleCancel = [&]() { ++cancelCommands; };
        snow_shot::presentation::WindowShortcutManager shortcuts;
        ScreenshotRecognitionWindow recognition(actions, &host, mode, &shortcuts);
        require(recognition.present({QGuiApplication::primaryScreen(), &host, host.rect(),
                                     QRectF(host.rect()), mode}),
                "cancellation fixture presents");
        auto presentation = std::make_shared<ScreenshotOcrPresentation>();
        ScreenshotOcrLine line;
        line.text = QStringLiteral("baseline");
        line.quad = ocrQuad(QRectF(40, 30, 150, 40));
        presentation->lines.push_back(line);
        recognition.setOcrPresentation(presentation);
        QApplication::processEvents();
        const auto begin = [&]() {
            presentation->selectAll();
            recognition.updateOcrSelection();
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(10, 10),
                              Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&recognition, &press);
            QMouseEvent move(QEvent::MouseMove, QPointF(210, 80), QPointF(210, 80), Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&recognition, &move);
            require(recognition.lineBoxSelectionActive(), "cancel fixture starts a box gesture");
        };
        for (const Qt::KeyboardModifiers modifiers :
             {Qt::KeyboardModifiers(Qt::NoModifier), Qt::KeyboardModifiers(Qt::AltModifier),
              Qt::KeyboardModifiers(Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier |
                                    Qt::MetaModifier)}) {
            begin();
            PhysicalKeyEvent escapePress(QEvent::KeyPress, Qt::Key_Escape, modifiers);
            PhysicalKeyEvent escapeRepeat(QEvent::KeyPress, Qt::Key_Escape, modifiers, QString(),
                                          true);
            PhysicalKeyEvent escapeRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(&recognition, &escapePress);
            QApplication::sendEvent(&recognition, &escapeRepeat);
            QApplication::sendEvent(&recognition, &escapeRelease);
            require(!recognition.lineBoxSelectionActive() && cancelCommands == 0 &&
                        presentation->selectedText() == QStringLiteral("baseline"),
                    "Escape cancels and consumes repeats and release despite changed modifiers");
        }
        for (const auto eventType : {QEvent::UngrabMouse, QEvent::WindowDeactivate}) {
            begin();
            QEvent event(eventType);
            QApplication::sendEvent(&recognition, &event);
            require(!recognition.lineBoxSelectionActive() &&
                        presentation->selectedText() == QStringLiteral("baseline"),
                    "capture loss and owner deactivation cancel only the uncommitted preview");
        }
        begin();
        QMouseEvent lostButton(QEvent::MouseMove, QPointF(200, 70), QPointF(200, 70), Qt::NoButton,
                               Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&recognition, &lostButton);
        require(!recognition.lineBoxSelectionActive(), "lost left-button state ends the gesture");
        begin();
        QMouseEvent outsideRelease(QEvent::MouseButtonRelease, QPointF(280, 160), QPointF(280, 160),
                                   Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(&recognition, &outsideRelease);
        require(!recognition.lineBoxSelectionActive() && !presentation->hasTextSelection() &&
                    QWidget::mouseGrabber() != &recognition,
                "release beyond the viewport commits final candidates and releases capture");
        begin();
        QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
        QApplication::sendEvent(&recognition, &focusOut);
        require(!recognition.lineBoxSelectionActive() && !presentation->hasTextSelection(),
                "focus loss cancels preview before applying ordinary selection clearing");
        begin();
        recognition.hide();
        require(!recognition.lineBoxSelectionActive(),
                "hiding recognition cancels pending capture");
        recognition.show();
        begin();
        recognition.setShowOriginalImage(true);
        require(!recognition.lineBoxSelectionActive() && !recognition.boxSelectionEnabled(),
                "showing the original image removes gesture state immediately");
        recognition.setShowOriginalImage(false);
        begin();
        require(
            recognition.updateSelectionGeometry(recognition.geometry(), QRectF(0, 0, 480, 240)) &&
                !recognition.lineBoxSelectionActive(),
            "a changed view transform cancels the old canvas-space rectangle");
        begin();
        recognition.setOcrPresentation(std::make_shared<ScreenshotOcrPresentation>());
        require(!recognition.lineBoxSelectionActive() && !presentation->lineBoxSelectionActive(),
                "replacing content cancels the retired presentation's preview");
    }
}

void ocrDoubleClickCopiesOnlyTheClickedBlock() {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >=
                    0,
                "load offscreen double-click font");
        QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
    }
#endif
    const snow_shot::storage::TextRecognitionSettings settings;
    const QString priorFormatting = settings.defaultFormatting();
    const QString priorPunctuation = settings.defaultPunctuation();
    const auto restore = qScopeGuard([&]() {
        settings.setDefaultFormatting(priorFormatting);
        settings.setDefaultPunctuation(priorPunctuation);
    });
    using Mode = ScreenshotRecognitionWindow::PresentationMode;
    using Rendering = ScreenshotOcrTextLayer::RenderingMode;
    for (const auto mode : {Mode::TopLevelWindow, Mode::EmbeddedChild}) {
        for (const auto rendering : {Rendering::Normal, Rendering::SelectionOnly}) {
            require(settings.setDefaultFormatting(QStringLiteral("keep")) &&
                        settings.setDefaultPunctuation(QStringLiteral("half")),
                    "configure unchanged OCR block copy");
            QWidget host;
            host.resize(320, 200);
            host.show();
            int copyCommands = 0;
            ScreenshotRecognitionWindowActions actions;
            actions.handleCopy = [&]() { ++copyCommands; };
            ScreenshotRecognitionWindow window(std::move(actions), &host, mode);
            const QRect canvasSelection(120, 80, 640, 400);
            require(window.present({QGuiApplication::primaryScreen(), &host, host.rect(),
                                    canvasSelection, mode}),
                    "scaled OCR double-click window presents");
            auto presentation = std::make_shared<ScreenshotOcrPresentation>();
            presentation->selection = canvasSelection;
            presentation->lines = {
                ScreenshotOcrLine{QStringLiteral("Neighbor"), 1.0,
                                  ocrQuad(QRectF(160, 100, 500, 40))},
                ScreenshotOcrLine{QStringLiteral("First, \u4e2d\u6587 e\u0301\nSecond! \U0001f642"),
                                  1.0, ocrQuad(QRectF(160, 180, 500, 120))},
                ScreenshotOcrLine{QString{}, 1.0, ocrQuad(QRectF(160, 340, 500, 40))},
            };
            presentation->lines[1].paragraph = true;
            presentation->lines[1].sourceLineQuads = {ocrQuad(QRectF(160, 180, 500, 40)),
                                                      ocrQuad(QRectF(160, 260, 500, 40))};
            presentation->prepareForRendering();
            window.setOcrPresentation(presentation, rendering);
            QApplication::processEvents();
            auto* layer = window.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
            require(layer != nullptr, "double-click fixture has a text layer");
            const auto items = ocrGlyphItems(layer);
            require(std::count_if(items.cbegin(), items.cend(),
                                  [](const auto* item) { return item->isVisible(); }) == 2,
                    "double-click fixtures render both nonempty text blocks");
            const auto doubleClickAt = [&](const QPoint& point,
                                           Qt::MouseButton button = Qt::LeftButton) {
                QWidget* receiver = window.childAt(point);
                require(receiver != nullptr, "double-click uses the actual overlay receiver");
                return doubleClickWidget(*receiver, receiver->mapFrom(&window, point), button);
            };
            const QPoint paragraphPoint(100, 65);
            const QString paragraph = presentation->lines[1].text;
            presentation->selectAll();
            window.updateOcrSelection();
            const QImage allSelected = layer->grab().toImage();
            QApplication::clipboard()->setText(QStringLiteral("sentinel"));
            require(doubleClickAt(paragraphPoint), "OCR double-click should consume the event");
            require(presentation->selectedText() == paragraph &&
                        !presentation->textSelectionActive() &&
                        QApplication::clipboard()->text() == paragraph && copyCommands == 0 &&
                        window.isVisible(),
                    "double-click replaces selection with the complete block and copies locally");
            require(layer->grab().toImage() != allSelected,
                    "double-click refreshes the rendered highlight to just the clicked block");
            QMouseEvent move(QEvent::MouseMove, QPointF(30, 20),
                             QPointF(window.mapToGlobal(QPoint(30, 20))), Qt::NoButton,
                             Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(&window, &move);
            require(presentation->selectedText() == paragraph,
                    "double-click finishes selection before subsequent pointer movement");
            require(doubleClickAt(QPoint(100, 20)) &&
                        presentation->selectedText() == QStringLiteral("Neighbor") &&
                        QApplication::clipboard()->text() == QStringLiteral("Neighbor"),
                    "unmerged OCR double-click copies just the clicked box");

            require(settings.setDefaultFormatting(QStringLiteral("remove")) &&
                        settings.setDefaultPunctuation(QStringLiteral("full")),
                    "configure OCR block formatting and punctuation defaults");
            doubleClickAt(paragraphPoint);
            require(
                QApplication::clipboard()->text() ==
                        QStringLiteral("First\uff0c \u4e2d\u6587 e\u0301Second\uff01 \U0001f642") &&
                    presentation->selectedText() == paragraph,
                "automatic copy applies OCR defaults without changing displayed text");

            window.setOcrCopyDefaultsEnabled(false);
            const QString partial = QStringLiteral("Translated, paragraph\n\U0001f642");
            window.updateOcrText(1, partial);
            doubleClickAt(paragraphPoint);
            require(QApplication::clipboard()->text() == partial &&
                        presentation->selectedText() == partial,
                    "translation double-click copies the displayed partial paragraph unchanged");
            const QString completed = partial + QStringLiteral(" complete!");
            window.updateOcrText(1, completed);
            require(QApplication::clipboard()->text() == partial,
                    "streaming updates do not replace already copied text");
            doubleClickAt(paragraphPoint);
            require(QApplication::clipboard()->text() == completed &&
                        presentation->selectedText() == completed && copyCommands == 0,
                    "a later double-click copies the current complete translation");

            for (const auto button : {Qt::RightButton, Qt::MiddleButton}) {
                QApplication::clipboard()->setText(QStringLiteral("sentinel"));
                doubleClickAt(paragraphPoint, button);
                require(QApplication::clipboard()->text() == QStringLiteral("sentinel"),
                        "non-left double-click never copies text");
            }
            for (const QPoint point : {QPoint(100, 140), QPoint(305, 175)}) {
                QApplication::clipboard()->setText(QStringLiteral("sentinel"));
                doubleClickAt(point);
                require(QApplication::clipboard()->text() == QStringLiteral("sentinel"),
                        "empty blocks and blank areas never fall back to copying all text");
            }
            window.setShowOriginalImage(true);
            doubleClickWidget(window, paragraphPoint);
            require(QApplication::clipboard()->text() == QStringLiteral("sentinel"),
                    "temporarily hidden recognition does not copy text");
        }
    }
}

void ocrDoubleClickPreservesEditorsAndResizeHandles() {
    QTextDocument document(QStringLiteral("First second paragraph"));
    bool resizeEnabled = true;
    bool resizing = false;
    int completedResizes = 0;
    ScreenshotRecognitionWindowActions actions;
    actions.selectionResizeDragMode = [&](const QPointF& point) {
        return resizeEnabled && point.y() < 40 ? ScreenshotSelectionDragMode::Top
                                               : ScreenshotSelectionDragMode::None;
    };
    actions.beginSelectionResize = [&](const QPointF&) {
        resizing = true;
        return true;
    };
    actions.finishSelectionResize = [&](const QPointF&) {
        resizing = false;
        ++completedResizes;
    };
    ScreenshotRecognitionWindow window(std::move(actions));
    require(window.present({QGuiApplication::primaryScreen(), nullptr, QRect(0, 0, 320, 200),
                            QRect(0, 0, 320, 200)}),
            "double-click exclusions fixture presents");
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = window.rect();
    presentation->lines = {
        ScreenshotOcrLine{QStringLiteral("Resize edge text"), 1.0,
                          ocrQuad(QRectF(20, 10, 200, 25))},
        ScreenshotOcrLine{QStringLiteral("Text below the resize handle"), 1.0,
                          ocrQuad(QRectF(20, 70, 200, 25))},
    };
    presentation->prepareForRendering();
    window.setOcrPresentation(presentation);
    QApplication::clipboard()->setText(QStringLiteral("sentinel"));
    doubleClickWidget(window, QPoint(100, 20));
    require(QApplication::clipboard()->text() == QStringLiteral("sentinel"),
            "resize handle double-click must not copy OCR text");
    require(!resizing && !presentation->hasTextSelection(),
            "resize handle double-click finishes resizing without selecting text");
    require(completedResizes == 1, "double-click preserves the first press's completed resize");
    QMouseEvent resizePress(QEvent::MouseButtonPress, QPointF(100, 20),
                            QPointF(window.mapToGlobal(QPoint(100, 20))), Qt::LeftButton,
                            Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &resizePress);
    QMouseEvent activeDoubleClick(QEvent::MouseButtonDblClick, QPointF(100, 80),
                                  QPointF(window.mapToGlobal(QPoint(100, 80))), Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &activeDoubleClick);
    require(resizing && !presentation->textSelectionActive() &&
                QApplication::clipboard()->text() == QStringLiteral("sentinel"),
            "an active resize retains input even when double-clicked over OCR text");
    QMouseEvent resizeRelease(QEvent::MouseButtonRelease, QPointF(100, 80),
                              QPointF(window.mapToGlobal(QPoint(100, 80))), Qt::LeftButton,
                              Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &resizeRelease);
    require(!resizing && completedResizes == 2, "active resize finishes on release");

    resizeEnabled = false;
    window.showTextEditor(&document);
    QApplication::processEvents();
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor != nullptr, "double-click editor fixture exists");
    QTextCursor cursor(&document);
    cursor.setPosition(9);
    editor->setTextCursor(cursor);
    doubleClickWidget(*editor->viewport(), editor->cursorRect().center());
    require(editor->textCursor().selectedText() == QStringLiteral("second") &&
                QApplication::clipboard()->text() == QStringLiteral("sentinel"),
            "editor double-click retains word selection without automatically copying");
}

void recognitionWindowCanExtendBeyondItsDpiScreen() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QWidget overlayHost;
    overlayHost.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlayHost.setGeometry(screen->geometry());
    overlayHost.show();

    const QRect screenGeometry = screen->geometry();
    const QRect crossScreenSelection(screenGeometry.right() - 100, screenGeometry.top() + 40, 240,
                                     120);
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    require(window.present(ScreenshotRecognitionWindow::Config{
                screen,
                &overlayHost,
                crossScreenSelection,
                QRectF(0.0, 0.0, 240.0, 120.0),
            }),
            "a recognition window should accept a selection spanning screen boundaries");
    QApplication::processEvents();
    require(window.geometry() == crossScreenSelection && window.parentWidget() == nullptr,
            "cross-screen recognition geometry must not be clipped by an overlay parent");
    require(window.windowHandle() != nullptr && window.windowHandle()->screen() == screen,
            "a cross-screen recognition window should retain the selection screen's DPI");
    require(window.minimumSize() == crossScreenSelection.size() &&
                window.maximumSize() == crossScreenSelection.size(),
            "recognition surfaces must disable native edge resizing");
    const QRect updatedSelection(crossScreenSelection.topLeft(), QSize(180, 90));
    require(window.updateSelectionGeometry(updatedSelection, QRectF(0, 0, 180, 90)),
            "selection resizing must still update a fixed recognition surface");
    require(window.geometry() == updatedSelection &&
                window.minimumSize() == updatedSelection.size() &&
                window.maximumSize() == updatedSelection.size(),
            "recognition size constraints must follow selection changes");
    window.hide();
    require(window.present({screen, &overlayHost, crossScreenSelection, QRectF(0, 0, 240, 120)}),
            "a pooled recognition surface must accept a new selection size");
    require(window.geometry() == crossScreenSelection,
            "reopening must replace the previous fixed size");
    window.hide();
}

#if defined(Q_OS_WIN) || defined(_WIN32)
QRect nativeClientGeometry(const QWidget& widget) {
    const DPI_AWARENESS_CONTEXT previousContext =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HWND hwnd = reinterpret_cast<HWND>(widget.winId());
    RECT clientRect{};
    POINT topLeft{};
    const bool queried =
        GetClientRect(hwnd, &clientRect) != FALSE && ClientToScreen(hwnd, &topLeft) != FALSE;
    if (previousContext != nullptr) {
        static_cast<void>(SetThreadDpiAwarenessContext(previousContext));
    }
    if (!queried) {
        return {};
    }
    return QRect(topLeft.x, topLeft.y, clientRect.right - clientRect.left,
                 clientRect.bottom - clientRect.top);
}

HWND windowAtPhysicalPoint(const POINT& point) {
    const DPI_AWARENESS_CONTEXT previousContext =
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HWND window = WindowFromPoint(point);
    if (previousContext != nullptr) {
        static_cast<void>(SetThreadDpiAwarenessContext(previousContext));
    }
    return window;
}

#endif

void recognitionMessageUsesOnlyItsPaintedShadow() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalTheme = themes.config();
    auto theme = originalTheme;
    theme.motion = false;
    themes.setConfig(theme);

    ScreenshotRecognitionWindow window({});
    const QRect geometry(80, 80, 480, 240);
    const ScreenshotRecognitionWindow::Config config{QGuiApplication::primaryScreen(), nullptr,
                                                     geometry,
                                                     QRectF(QPointF(), QSizeF(geometry.size()))};
    for (int presentation = 0; presentation < 2; ++presentation) {
        require(window.present(config), "the recognition message owner should be presented");
#ifdef Q_OS_MACOS
        if (QGuiApplication::platformName() == QStringLiteral("cocoa")) {
            NSWindow* nativeWindow = reinterpret_cast<NSView*>(window.winId()).window;
            require(nativeWindow != nil && !nativeWindow.hasShadow,
                    "Cocoa must not add an outline around painted message shadow pixels");
        }
#endif
        require(window.windowFlags().testFlag(Qt::NoDropShadowWindowHint),
                "the transparent recognition surface must not shadow the message's shadow");
        // The recognition surface deliberately retains a nearly transparent fill
        // for Windows hit testing; preserve that background while adding the message.
        const QImage background = window.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        adqt::widgets::AdMessage messages(&window);
        auto* message = messages.loading(QStringLiteral("Recognizing text"), 0);
        require(message != nullptr, "the recognition surface should display a loading message");
        QApplication::processEvents();
        const QImage image = window.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        bool hasPaintedShadow = false;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const int alpha = qAlpha(image.pixel(x, y));
                hasPaintedShadow |= alpha > qAlpha(background.pixel(x, y)) && alpha < 255;
            }
        }
        require(hasPaintedShadow, "disabling the native shadow must preserve the painted shadow");
        require(image.pixel(0, 0) == background.pixel(0, 0),
                "the recognition surface background should be preserved outside the message");
        window.hide();
    }
    themes.setConfig(originalTheme);
}

void recognitionWindowUsesOrdinaryQtWindowBehavior(bool nativeSurfaceOnly = false) {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    const QRect physicalScreen = ScreenshotGeometryMapper::physicalRectForScreen(*screen);
    const QRect nativeSelection(physicalScreen.topLeft() + QPoint(31, 29), QSize(420, 240));
    const QRect logicalSelection =
        ScreenshotGeometryMapper::logicalRectForPhysicalRect(nativeSelection, screen);
    const QRectF canvasSelection(QPointF(0.0, 0.0), QSizeF(nativeSelection.size()));
    QWidget overlayHost;
    overlayHost.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlayHost.setGeometry(screen->geometry());
    overlayHost.show();
    overlayHost.raise();
    int recognitionCancelCalls = 0;
    int recognitionCopyCalls = 0;
    int textEditedCalls = 0;
    ScreenshotRecognitionWindowActions actions;
    actions.handleCancel = [&recognitionCancelCalls]() { ++recognitionCancelCalls; };
    actions.handleCopy = [&recognitionCopyCalls]() { ++recognitionCopyCalls; };
    actions.handleTextEdited = [&textEditedCalls](const QString&) { ++textEditedCalls; };
    int tableStateChanges = 0;
    ScreenshotTableCommandState latestTableState;
    actions.handleTableCommandStateChanged =
        [&tableStateChanges, &latestTableState](const ScreenshotTableCommandState& state) {
            ++tableStateChanges;
            latestTableState = state;
        };
    int rejectedOperations = 0;
    actions.handleTableOperationRejected = [&rejectedOperations](const QString&) {
        ++rejectedOperations;
    };
    snow_shot::presentation::WindowShortcutManager shortcutManager;
    shortcutManager.addScopeWindow(&overlayHost);
    int sessionShortcutCalls = 0;
    snow_shot::presentation::WindowShortcutManager::Binding sessionShortcut;
    sessionShortcut.id = QStringLiteral("test.session.shortcut");
    sessionShortcut.keyCombinations = {
        QKeyCombination(Qt::NoModifier, Qt::Key_P),
    };
    sessionShortcut.activate = [&sessionShortcutCalls](const auto&) {
        ++sessionShortcutCalls;
        return true;
    };
    int lowerPriorityCopyCalls = 0;
    snow_shot::presentation::WindowShortcutManager::Binding lowerPriorityCopy;
    lowerPriorityCopy.id = QStringLiteral("test.session.copy");
    lowerPriorityCopy.keyCombinations = {
        QKeyCombination(Qt::ControlModifier, Qt::Key_C),
    };
    lowerPriorityCopy.priority =
        snow_shot::presentation::WindowShortcutManager::StandardPriority::ScreenshotShortcut;
    lowerPriorityCopy.activate = [&lowerPriorityCopyCalls](const auto&) {
        ++lowerPriorityCopyCalls;
        return true;
    };
    require(shortcutManager.addBinding(&overlayHost, std::move(sessionShortcut)) != 0,
            "shared screenshot shortcut registration should succeed");
    require(shortcutManager.addBinding(&overlayHost, std::move(lowerPriorityCopy)) != 0,
            "lower-priority screenshot copy registration should succeed");
    ScreenshotRecognitionWindow window(
        std::move(actions), nullptr, ScreenshotRecognitionWindow::PresentationMode::TopLevelWindow,
        &shortcutManager);
    require(window.present(ScreenshotRecognitionWindow::Config{
                screen,
                &overlayHost,
                logicalSelection,
                canvasSelection,
            }),
            "recognition window should present a valid selection");
    QApplication::processEvents();

    require(window.isWindow() && window.parentWidget() == nullptr,
            "the recognition host should be an ordinary top-level Qt window");
    require(window.windowHandle() != nullptr &&
                window.windowHandle()->transientParent() == overlayHost.windowHandle(),
            "the screenshot overlay should own recognition stacking through Qt");
    PhysicalKeyEvent sessionShortcutEvent(QEvent::KeyPress, Qt::Key_P, Qt::NoModifier);
    QApplication::sendEvent(&window, &sessionShortcutEvent);
    require(sessionShortcutCalls == 1 && sessionShortcutEvent.isAccepted(),
            "a focused recognition surface should dispatch through its shared screenshot manager");
    require(window.geometry() == logicalSelection,
            "the recognition window should preserve the mapped selection geometry");
    require(window.windowHandle() != nullptr && window.windowHandle()->screen() == screen,
            "the recognition window DPI should come from the configured selection screen");
    require(window.findChild<SnowCanvasWidget*>() == nullptr,
            "the recognition window should not create a screenshot canvas");
    require(window.testAttribute(Qt::WA_TranslucentBackground) && !window.autoFillBackground(),
            "the recognition window should have a visually transparent, input-bearing surface");

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = canvasSelection.toAlignedRect();
    ScreenshotOcrLine recognizedLine;
    recognizedLine.text = QStringLiteral("Selectable text");
    const QPointF selectionCenter = canvasSelection.center();
    recognizedLine.quad = QPolygonF({
        selectionCenter + QPointF(-90.0, -18.0),
        selectionCenter + QPointF(90.0, -18.0),
        selectionCenter + QPointF(90.0, 18.0),
        selectionCenter + QPointF(-90.0, 18.0),
    });
    presentation->lines.push_back(recognizedLine);
    presentation->prepareForRendering();
    window.setOcrPresentation(presentation);
    QApplication::processEvents();
    auto* textLayer = window.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
    require(textLayer != nullptr && textLayer->isVisible() && textLayer->scene() != nullptr &&
                ocrGlyphItems(textLayer).size() == 1,
            "selectable OCR text should be hosted directly by the transparent window");
    require(textLayer->testAttribute(Qt::WA_TransparentForMouseEvents) &&
                textLayer->viewport()->testAttribute(Qt::WA_TransparentForMouseEvents),
            "the OCR rendering child should leave all pointer input to its recognition window");

    const auto localForCanvas = [&window, &canvasSelection](const QPointF& canvasPosition) {
        return QPointF((canvasPosition.x() - canvasSelection.left()) * window.width() /
                           canvasSelection.width(),
                       (canvasPosition.y() - canvasSelection.top()) * window.height() /
                           canvasSelection.height());
    };
    const QPointF blankCanvasPosition = canvasSelection.topLeft() + QPointF(12.0, 12.0);
    const QPointF blankLocalPosition = localForCanvas(blankCanvasPosition);
    if (nativeSurfaceOnly) {
#if defined(Q_OS_WIN) || defined(_WIN32)
        const QRect actualClientGeometry = nativeClientGeometry(window);
        require(actualClientGeometry.isValid() && !actualClientGeometry.isEmpty(),
                "the recognition window should expose a valid native client surface");
        window.repaint();
        QApplication::processEvents();
        const QRect actualNative = nativeClientGeometry(window);
        const POINT blankNativePosition{
            actualNative.left() +
                qRound(blankLocalPosition.x() * actualNative.width() / window.width()),
            actualNative.top() +
                qRound(blankLocalPosition.y() * actualNative.height() / window.height()),
        };
        const HWND blankOwner = windowAtPhysicalPoint(blankNativePosition);
        const HWND recognitionHwnd = reinterpret_cast<HWND>(window.winId());
        require(blankOwner == recognitionHwnd,
                "transparent blank pixels should remain part of the recognition input surface");
#endif
        window.hide();
        return;
    }
    const QPointF localStart = localForCanvas(selectionCenter + QPointF(-70.0, 0.0));
    const QPointF localEnd = localForCanvas(selectionCenter + QPointF(70.0, 0.0));
    QMouseEvent recognitionPress(QEvent::MouseButtonPress, localStart,
                                 window.mapToGlobal(localStart.toPoint()), Qt::LeftButton,
                                 Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &recognitionPress);
    require(presentation->textSelectionActive(),
            "the recognition window should begin OCR selection without controller forwarding");
    QMouseEvent recognitionMove(QEvent::MouseMove, localEnd, window.mapToGlobal(localEnd.toPoint()),
                                Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &recognitionMove);
    QMouseEvent recognitionRelease(QEvent::MouseButtonRelease, localEnd,
                                   window.mapToGlobal(localEnd.toPoint()), Qt::LeftButton,
                                   Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &recognitionRelease);
    require(!presentation->textSelectionActive() && !presentation->selectedText().isEmpty(),
            "the recognition window should update and finish OCR selection locally");

    PhysicalKeyEvent selectAll(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    QApplication::sendEvent(&window, &selectAll);
    require(presentation->selectedText() == recognizedLine.text,
            "Select All should be handled locally by the recognition window");
    PhysicalKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(&window, &copy);
    require(lowerPriorityCopyCalls == 0 && copy.isAccepted() &&
                QGuiApplication::clipboard()->text() == recognizedLine.text &&
                recognitionCopyCalls == 1,
            "recognition copy must win over the lower-priority screenshot copy binding and end "
            "the screenshot");

    QMouseEvent blankPress(QEvent::MouseButtonPress, blankLocalPosition,
                           window.mapToGlobal(blankLocalPosition.toPoint()), Qt::LeftButton,
                           Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &blankPress);
    QMouseEvent blankRelease(QEvent::MouseButtonRelease, blankLocalPosition,
                             window.mapToGlobal(blankLocalPosition.toPoint()), Qt::LeftButton,
                             Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&window, &blankRelease);
    require(!presentation->hasTextSelection(),
            "clicking a blank OCR area should clear the existing text selection");

    // Selection is custom presentation state, so losing focus must explicitly clear it.
    PhysicalKeyEvent selectAllAgain(QEvent::KeyPress, Qt::Key_A, Qt::ControlModifier);
    QApplication::sendEvent(&window, &selectAllAgain);
    require(presentation->hasTextSelection(),
            "Select All should establish a selection before the focus-loss check");
    QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
    QApplication::sendEvent(&window, &focusOut);
    require(!presentation->hasTextSelection(),
            "losing recognition-window focus should clear the existing text selection");

    QTextDocument editableDocument;
    QFont oversizedFont = editableDocument.defaultFont();
    oversizedFont.setPixelSize(32);
    editableDocument.setDefaultFont(oversizedFont);
    editableDocument.setPlainText(QStringLiteral("Editable recognized text"));
    window.showTextEditor(&editableDocument);
    QApplication::processEvents();
    auto* textEditor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    auto* textEditorContainer =
        window.findChild<QWidget*>(QStringLiteral("screenshotOcrEditorContainer"));
    auto* adTextEditor = qobject_cast<adqt::widgets::AdTextEdit*>(textEditor);
    require(textEditorContainer != nullptr && textEditor != nullptr &&
                textEditor->parentWidget() == textEditorContainer &&
                textEditorContainer->palette().color(QPalette::Window) ==
                    adqt::theme::ThemeManager::instance()
                        .resolveTheme(textEditorContainer)
                        .colorBgContainer &&
                adTextEditor != nullptr && textEditor->frameStyle() == QFrame::NoFrame &&
                adTextEditor->variant() == adqt::widgets::AdTextEdit::Variant::Borderless,
            "the OCR text editor should use a themed borderless container");

    QTextCursor noTextSelection = textEditor->textCursor();
    noTextSelection.clearSelection();
    textEditor->setTextCursor(noTextSelection);
    PhysicalKeyEvent copyWholeDraft(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(textEditor, &copyWholeDraft);
    require(copyWholeDraft.isAccepted() &&
                QGuiApplication::clipboard()->text() ==
                    QStringLiteral("Editable recognized text") &&
                recognitionCopyCalls == 2,
            "text-recognition edit mode should copy the complete draft when no text is selected "
            "and end the screenshot");

    auto* overlayScrollBar =
        textEditor->findChild<QScrollBar*>(QStringLiteral("adtextarea-overlay-vbar"));
    require(overlayScrollBar != nullptr && textEditor->verticalScrollBar()->isHidden(),
            "the OCR text editor should use an overlay scrollbar without reserving layout width");
    const QRect viewportGeometry = textEditor->viewport()->geometry();
    const int viewportLeftInset = viewportGeometry.left();
    const int viewportRightInset = textEditor->width() - viewportGeometry.right() - 1;
    require(viewportLeftInset > 0 && viewportRightInset > 0 &&
                viewportLeftInset == viewportRightInset && editableDocument.documentMargin() == 0.0,
            "the OCR text editor should use symmetric viewport content margins");
    const int viewportWidthBeforeOverflow = textEditor->viewport()->width();
    editableDocument.setPlainText(QString(5000, QLatin1Char('x')));
    QApplication::processEvents();
    require(overlayScrollBar->isVisible() &&
                textEditor->viewport()->width() == viewportWidthBeforeOverflow,
            "the OCR overlay scrollbar should not change the text viewport width");
    const auto theme = adqt::theme::ThemeManager::instance().resolveTheme(textEditor);
    const int themeFontSize = qRound(theme.fontSize);
    require(textEditor->font().pixelSize() == themeFontSize &&
                editableDocument.defaultFont().pixelSize() == themeFontSize,
            "the OCR text editor should use the theme standard font size");

    window.showTextEditor(&editableDocument, true, true);
    QApplication::processEvents();
    auto* translationSpin =
        window.findChild<adqt::widgets::AdSpin*>(QStringLiteral("screenshotOcrTranslationSpin"));
    require(textEditor->isReadOnly() && translationSpin != nullptr &&
                translationSpin->isVisible() && translationSpin->spinning(),
            "streaming translation should make the editor read-only and show its spinner");
    window.setTextEditorStreaming(false);
    QApplication::processEvents();
    require(!textEditor->isReadOnly() && !translationSpin->isVisible() &&
                !translationSpin->spinning(),
            "translation completion should restore editing and hide the spinner");

    textEditedCalls = 0;
    textEditedCalls = 0;
    window.hideTextEditor();
    require(textEditedCalls == 0,
            "hiding the OCR text editor should not emit a spurious empty text edit");

    auto session = std::make_shared<ScreenshotTableEditingSession>(
        ScreenshotTableDocument::fromPlainText(QStringLiteral("A\tB\nC\tD")));
    window.setTableSession(session);
    QApplication::processEvents();
    auto* editor =
        window.findChild<ScreenshotTableEditor*>(QStringLiteral("snowShotRecognizedTable"));
    require(editor != nullptr && editor->geometry() == window.rect(),
            "the table editor should fill the recognition window");
    require(editor->selectionModel()->selectedIndexes().isEmpty() &&
                !editor->selectedRange().isValid() && !editor->commandState().hasSelection,
            "a newly recognized table should not select a cell by default");
    require(editor->copySelectionToClipboard() &&
                QApplication::clipboard()->text() == QStringLiteral("A\tB\nC\tD"),
            "copying a newly recognized table should copy the complete table");
    const auto tableTheme = adqt::theme::ThemeManager::instance().resolveTheme(editor);
    const int tablePadding = std::max(0, qRound(tableTheme.sizeSM));
    require(editor->viewport()->geometry().topLeft() == QPoint(tablePadding, tablePadding),
            "the table viewport should keep theme-driven outer padding");
    auto* horizontalScrollBar =
        qobject_cast<adqt::widgets::AdScrollBar*>(editor->horizontalScrollBar());
    auto* verticalScrollBar =
        qobject_cast<adqt::widgets::AdScrollBar*>(editor->verticalScrollBar());
    require(horizontalScrollBar != nullptr && verticalScrollBar != nullptr,
            "the table should use default ant_design_qt scrollbars on both axes");
    require(textLayer->isHidden(), "table mode should hide selectable OCR text");
    require(editor->rowSpan(0, 0) == 1 && editor->columnSpan(0, 0) == 1,
            "unmerged recognition cells should use normal table geometry");

    const QSize viewportBeforeEdit = editor->viewport()->size();
    const int firstColumnWidthBeforeEdit = editor->columnWidth(0);
    const int secondColumnWidthBeforeEdit = editor->columnWidth(1);
    const int firstRowHeightBeforeEdit = editor->rowHeight(0);

    clickCell(*editor, 0, 0);
    auto* cellEditor =
        editor->findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
    require(cellEditor != nullptr && editor->isEditingCell(),
            "one click should open an inline editor directly over the selected cell");
    auto* cellScrollBar =
        qobject_cast<adqt::widgets::AdScrollBar*>(cellEditor->verticalScrollBar());
    require(cellScrollBar != nullptr &&
                cellScrollBar->scrollBarThickness() == verticalScrollBar->scrollBarThickness(),
            "inline table textareas should use the default ant_design_qt scrollbar");
    require(editor->viewport()->size() == viewportBeforeEdit &&
                editor->columnWidth(0) == firstColumnWidthBeforeEdit &&
                editor->columnWidth(1) == secondColumnWidthBeforeEdit &&
                editor->rowHeight(0) == firstRowHeightBeforeEdit,
            "opening an inline editor should preserve table and viewport geometry");
    require(cellEditor->document()->documentMargin() == 0.0,
            "inline table editors should remove the document's implicit text margin");
    PhysicalKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(cellEditor, &escape);
    processEditorClose();
    PhysicalKeyEvent escapeRepeat(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier, QString(),
                                  true);
    QApplication::sendEvent(editor, &escapeRepeat);
    PhysicalKeyEvent escapeRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(editor, &escapeRelease);
    require(!editor->isEditingCell() && recognitionCancelCalls == 0,
            "Escape should cancel an active cell edit before reaching screenshot cancellation");

    clickCell(*editor, 0, 0);
    cellEditor = editor->findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
    require(cellEditor != nullptr, "inline editor should reopen after cancel");
    cellEditor->setPlainText(QString(240, QLatin1Char('L')));
    PhysicalKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(cellEditor, &enter);
    processEditorClose();
    require(session->document.cellText(0, 0) == QString(240, QLatin1Char('L')) &&
                editor->currentIndex().row() == 1 && editor->currentIndex().column() == 0,
            "Enter should commit the edit and continue vertically in the same column");
    require(editor->viewport()->size() == viewportBeforeEdit &&
                editor->columnWidth(0) == firstColumnWidthBeforeEdit &&
                editor->columnWidth(1) == secondColumnWidthBeforeEdit &&
                editor->rowHeight(0) == firstRowHeightBeforeEdit,
            "committing a cell edit should not resize the table around the next editor");
    require(editor->cancelActiveEdit(), "the continued inline edit should be cancellable");
    processEditorClose();

    window.resetTable();
    clickCell(*editor, 0, 0);
    cellEditor = editor->findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
    require(cellEditor != nullptr, "inline editor should open for horizontal navigation");
    PhysicalKeyEvent tab(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    QApplication::sendEvent(cellEditor, &tab);
    processEditorClose();
    require(editor->currentIndex().row() == 0 && editor->currentIndex().column() == 1 &&
                editor->isEditingCell(),
            "Tab should commit and continue horizontally in the next column");
    require(editor->cancelActiveEdit(), "the horizontally continued edit should be cancellable");
    processEditorClose();

    clickCell(*editor, 0, 0);
    cellEditor = editor->findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
    require(cellEditor != nullptr, "inline editor should open before toolbar-style copying");
    cellEditor->setPlainText(QStringLiteral("Edited before copy"));
    window.commitActiveTableEdit();
    require(session->document.cellText(0, 0) == QStringLiteral("Edited before copy"),
            "copy preparation should synchronously commit the active cell editor");
    require(!editor->isEditingCell(),
            "copy preparation should synchronously close the active cell editor");
    processEditorClose();
    window.resetTable();

    editor->selectAll();
    QApplication::processEvents();
    require(window.tableCommandState().canMerge,
            "a rectangular multi-cell selection should enable Merge");
    window.mergeTableSelection();
    require(session->document.anchorCellAt(0, 0)->rowSpan == 2 &&
                session->document.anchorCellAt(0, 0)->columnSpan == 2 &&
                editor->rowSpan(0, 0) == 2 && editor->columnSpan(0, 0) == 2,
            "Merge should update both document spans and visible table geometry");
    require(window.tableCommandState().canSplit && window.tableCommandState().canUndo,
            "a merged table should enable Split and Undo");

    auto& themeManager = adqt::theme::ThemeManager::instance();
    const adqt::theme::AdTheme originalTheme = themeManager.theme();
    themeManager.setPreset(adqt::theme::ThemeScheme::Light, adqt::theme::ThemeDensity::Compact);
    QApplication::processEvents();
    const QImage light1x = renderTableEditor(*editor, 1.0);
    const QImage light2x = renderTableEditor(*editor, 2.0);
    themeManager.setPreset(adqt::theme::ThemeScheme::Dark, adqt::theme::ThemeDensity::Compact);
    QApplication::processEvents();
    const QImage dark1x = renderTableEditor(*editor, 1.0);
    const QImage dark2x = renderTableEditor(*editor, 2.0);
    themeManager.setTheme(originalTheme);
    QApplication::processEvents();
    require(!light1x.isNull() && !dark1x.isNull() &&
                averageLightness(light1x) > averageLightness(dark1x) + 40.0,
            "light and dark theme tokens should produce distinct nonblank table surfaces");
    require(light2x.size() == light1x.size() * 2 && dark2x.size() == dark1x.size() * 2,
            "table rendering should preserve logical geometry at high device pixel ratios");
    const QString snapshotDirectory = qEnvironmentVariable("SNOW_SHOT_TABLE_SNAPSHOT_DIR");
    if (!snapshotDirectory.isEmpty() && QDir().mkpath(snapshotDirectory)) {
        light1x.save(QDir(snapshotDirectory).filePath(QStringLiteral("table-light-1x.png")));
        light2x.save(QDir(snapshotDirectory).filePath(QStringLiteral("table-light-2x.png")));
        dark1x.save(QDir(snapshotDirectory).filePath(QStringLiteral("table-dark-1x.png")));
        dark2x.save(QDir(snapshotDirectory).filePath(QStringLiteral("table-dark-2x.png")));
    }

    window.undoTableEdit();
    require(session->document == session->baseline && window.tableCommandState().canRedo,
            "Undo should restore the recognized baseline and enable Redo");
    window.redoTableEdit();
    require(session->document.anchorCellAt(0, 0)->rowSpan == 2,
            "Redo should restore the merged table");
    window.splitTableSelection();
    require(session->document.anchorCellAt(0, 0)->rowSpan == 1 &&
                session->document.cellText(0, 0) == QStringLiteral("A\nB\nC\nD") &&
                session->document.cellText(1, 1).isEmpty(),
            "Split should retain merged text at top-left and blank uncovered cells");
    require(window.tableCommandState().canReset,
            "editing recognized structure should enable Reset");
    window.resetTable();
    require(session->document == session->baseline && !window.tableCommandState().canReset,
            "Reset should restore recognized values and spans without changing dimensions");

    editor->setCurrentIndex(editor->model()->index(1, 1));
    editor->selectionModel()->clearSelection();
    editor->copySelection();
    QApplication::processEvents();
    require(QApplication::clipboard()->text() == QStringLiteral("A\tB\nC\tD") &&
                recognitionCopyCalls == 3,
            "table recognition should copy the complete table when no cells are selected and end "
            "the screenshot");
    editor->setCurrentIndex(editor->model()->index(1, 1));
    editor->pasteSelection();
    require(rejectedOperations == 1 && session->document == session->baseline,
            "Paste should reject data that does not fit fixed recognized dimensions");
    editor->selectAll();
    editor->clearSelectionContents();
    editor->setCurrentIndex(editor->model()->index(0, 0));
    editor->pasteSelection();
    require(session->document.rowCount() == 2 && session->document.columnCount() == 2 &&
                session->document.cellText(1, 1) == QStringLiteral("D"),
            "a fitting paste should replace cells without inserting rows or columns");
    window.undoTableEdit();
    require(session->document.toPlainText() == QStringLiteral("\t\n\t"),
            "a pasted range should be reversible as one undo command");
    window.redoTableEdit();
    require(session->document == session->baseline,
            "Redo should restore the pasted range without changing dimensions");
    window.undoTableEdit();
    window.undoTableEdit();
    require(session->document == session->baseline,
            "undoing Paste and Clear should return to the recognized baseline");

    window.clearTableSession();
    window.setTableSession(session);
    QApplication::processEvents();
    editor = window.findChild<ScreenshotTableEditor*>(QStringLiteral("snowShotRecognizedTable"));
    require(editor != nullptr && editor->commandState().canRedo &&
                editor->selectedRange().isValid(),
            "table re-entry should restore the session selection and undo history");
    require(tableStateChanges > 0 && latestTableState.hasSelection,
            "table command availability should be published to the toolbar controller");

    window.hide();
    QApplication::processEvents();
}

QGraphicsTextItem* formattedTextItem(QGraphicsView* layer) {
    if (layer == nullptr || layer->scene() == nullptr) {
        return nullptr;
    }
    for (QGraphicsItem* item : layer->scene()->items()) {
        auto* textItem = dynamic_cast<QGraphicsTextItem*>(item);
        if (textItem != nullptr &&
            textItem->objectName() == QStringLiteral("screenshotClipboardTextItem")) {
            return textItem;
        }
    }
    return nullptr;
}

void formattedTextHoverUpdatesCursorWithoutClicking() {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >=
                    0,
                "load the formatted hover test font");
    }
#endif
    using Mode = ScreenshotRecognitionWindow::PresentationMode;
    using Rendering = ScreenshotOcrTextLayer::RenderingMode;
    for (const auto mode : {Mode::TopLevelWindow, Mode::EmbeddedChild}) {
        for (const auto rendering : {Rendering::Normal, Rendering::SelectionOnly}) {
            QWidget host;
            host.resize(300, 180);
            host.show();
            ScreenshotRecognitionWindow recognition({}, &host, mode);
            require(recognition.present({QGuiApplication::primaryScreen(), &host, host.rect(),
                                         QRectF(QPointF(), QSizeF(host.size()) * 2.0), mode, 2.0}),
                    "present formatted text hover at a scaled canvas");
            auto document = std::make_shared<QTextDocument>();
            document->setHtml(QStringLiteral("<p><b>Hover</b> text</p>"));
            document->setTextWidth(140);
            recognition.showFormattedText(document, rendering, false);
            QApplication::processEvents();
            auto* layer =
                recognition.findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText"));
            auto* item = formattedTextItem(layer);
            require(item != nullptr, "formatted hover requires a selectable document");
            const auto block = document->firstBlock();
            const auto line = block.layout()->lineAt(0);
            const QPoint text = layer->mapFromScene(
                item->mapToScene(block.layout()->position() +
                                 QPointF(line.cursorToX(2), line.y() + line.height() / 2)));
            const QPoint blank = layer->viewport()->rect().bottomRight() - QPoint(20, 20);
            const auto hover = [&](QPoint point, Qt::CursorShape cursor) {
                QMouseEvent move(QEvent::MouseMove, QPointF(point),
                                 layer->viewport()->mapToGlobal(point), Qt::NoButton, Qt::NoButton,
                                 Qt::NoModifier);
                QApplication::sendEvent(layer->viewport(), &move);
                require(layer->viewport()->cursor().shape() == cursor,
                        "formatted hover must distinguish text from background without clicking");
                require(!item->textCursor().hasSelection() && !recognition.textSelectionActive(),
                        "formatted hover must not begin a selection");
            };
            hover(text, Qt::IBeamCursor);
            hover(blank, Qt::ArrowCursor);
            hover(text, Qt::IBeamCursor);
        }
    }
}

QImage renderFormattedTextItem(QGraphicsTextItem* item, bool focused) {
    const QRectF bounds = item->boundingRect();
    QImage image(bounds.size().toSize(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.translate(-bounds.topLeft());
    QStyleOptionGraphicsItem option;
    option.exposedRect = bounds;
    option.state = focused ? QStyle::State_Enabled | QStyle::State_HasFocus : QStyle::State_Enabled;
    item->paint(&painter, &option, nullptr);
    return image;
}

QRect nonWhitePixelBounds(const QImage& image) {
    QRect bounds;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.red() < 250 || color.green() < 250 || color.blue() < 250) {
                bounds = bounds.isNull() ? QRect(x, y, 1, 1) : bounds.united(QRect(x, y, 1, 1));
            }
        }
    }
    return bounds;
}

void shortRecognitionWindowPreservesExactSelectionGeometryAcrossModes() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    constexpr int selectionWidth = 260;
    constexpr int selectionHeight = 24;
    const QRect selectionGeometry(screen->geometry().topLeft() + QPoint(40, 40),
                                  QSize(selectionWidth, selectionHeight));
    const QRectF canvasSelection(QPointF(), QSizeF(selectionWidth, selectionHeight));

    QWidget overlayHost;
    overlayHost.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlayHost.setGeometry(screen->geometry());
    overlayHost.show();

    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    require(window.present(ScreenshotRecognitionWindow::Config{
                screen,
                &overlayHost,
                selectionGeometry,
                canvasSelection,
            }),
            "a short recognition selection should be presentable");

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = canvasSelection.toAlignedRect();
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Short OCR");
    line.quad = QPolygonF({
        QPointF(70.0, 4.0),
        QPointF(190.0, 4.0),
        QPointF(190.0, 20.0),
        QPointF(70.0, 20.0),
    });
    presentation->lines.push_back(line);
    presentation->prepareForRendering();
    window.setOcrPresentation(presentation);
    QApplication::processEvents();

    auto* textLayer = window.findChild<QGraphicsView*>(QStringLiteral("snowShotOcrTextLayer"));
    require(window.geometry() == selectionGeometry && textLayer != nullptr &&
                textLayer->geometry() == window.rect(),
            "OCR display mode must not enlarge a short screenshot selection");
    const QList<QGraphicsItem*> textItems = ocrGlyphItems(textLayer);
    require(textItems.size() == 1 &&
                std::abs(textItems.constFirst()->sceneBoundingRect().center().y() -
                         selectionHeight / 2.0) < 0.5,
            "OCR text must retain its canvas vertical position in a short selection");

    QTextDocument document;
    document.setPlainText(QStringLiteral("Editable short OCR text"));
    window.showTextEditor(&document);
    QApplication::processEvents();

    auto* editorContainer =
        window.findChild<QWidget*>(QStringLiteral("screenshotOcrEditorContainer"));
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(window.geometry() == selectionGeometry && editorContainer != nullptr &&
                editorContainer->geometry() == window.rect() && editor != nullptr &&
                editor->geometry() == editorContainer->rect(),
            "edit mode must remain exactly within a short screenshot selection");
    window.hideTextEditor();
}

void textRecognitionEditorsCollapseMarginsInSmallAreas() {
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalTheme = themes.theme();
    const auto restoreTheme = qScopeGuard([&] { themes.setTheme(originalTheme); });
    themes.setPreset(adqt::theme::ThemeScheme::Light, adqt::theme::ThemeDensity::Compact);
    for (const auto mode : {ScreenshotRecognitionWindow::PresentationMode::TopLevelWindow,
                            ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild}) {
        QWidget parent;
        parent.resize(400, 240);
        parent.show();
        QTextDocument document;
        document.setPlainText(QStringLiteral("Editable recognition text"));
        ScreenshotRecognitionWindow window({}, &parent, mode);
        window.resize(320, 24);
        window.showTextEditor(&document);
        window.show();
        QApplication::processEvents();
        auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
        require(editor != nullptr && editor->geometry() == window.rect() &&
                    editor->viewport()->geometry() == editor->rect(),
                "a short recognition editor must give its entire area to text");

        window.resize(320, 200);
        QApplication::processEvents();
        const QRect paddedViewport = editor->viewport()->geometry();
        require(paddedViewport.left() > 0 && paddedViewport.top() > 0 &&
                    paddedViewport.width() < editor->width() &&
                    paddedViewport.height() < editor->height(),
                "a roomy recognition editor must retain its themed padding");

        QTextCursor selection(&document);
        selection.setPosition(0);
        selection.setPosition(8, QTextCursor::KeepAnchor);
        editor->setTextCursor(selection);
        for (const QSize& size : {QSize(320, 24), QSize(80, 200), QSize(80, 24)}) {
            window.resize(size);
            QApplication::processEvents();
            require(editor->geometry() == window.rect() &&
                        editor->viewport()->geometry() == editor->rect() &&
                        editor->textCursor().selectedText() == QStringLiteral("Editable"),
                    "resizing a short or narrow editor must remove margins and preserve selection");

            window.showTextEditor(&document, true, true);
            document.setPlainText(QStringLiteral("Streaming translation result"));
            QApplication::processEvents();
            require(editor->isReadOnly() && editor->viewport()->geometry() == editor->rect(),
                    "streaming translation must retain the compact text area after updates");
            window.setTextEditorStreaming(false);
            QApplication::processEvents();
            require(!editor->isReadOnly() && editor->viewport()->geometry() == editor->rect(),
                    "completed translation must retain the compact text area");

#if SNOW_SHOT_ENABLE_LATEX_RECOGNITION
            window.showLatexEditor(&document);
            QApplication::processEvents();
            require(editor->viewport()->geometry() == editor->rect(),
                    "LaTeX source editing must share the compact text area");
#endif
            for (const auto scheme :
                 {adqt::theme::ThemeScheme::Dark, adqt::theme::ThemeScheme::Light}) {
                themes.setPreset(scheme, adqt::theme::ThemeDensity::Compact);
                QApplication::processEvents();
                require(editor->viewport()->geometry() == editor->rect(),
                        "theme refreshes must not restore padding in a small text area");
            }
            window.resize(320, 200);
            QApplication::processEvents();
            require(editor->viewport()->geometry() == paddedViewport,
                    "growing the recognition editor must restore its original themed padding");
            window.showTextEditor(&document);
            document.setPlainText(QStringLiteral("Editable recognition text"));
            selection = QTextCursor(&document);
            selection.setPosition(0);
            selection.setPosition(8, QTextCursor::KeepAnchor);
            editor->setTextCursor(selection);
        }
        window.hideTextEditor();
    }

    adqt::widgets::AdTextEdit formEditor;
    formEditor.setHeightMode(adqt::widgets::AdTextEdit::HeightMode::FixedGeometry);
    formEditor.resize(80, 24);
    formEditor.show();
    QApplication::processEvents();
    require(formEditor.viewport()->geometry().left() > 0,
            "an outlined form editor must retain its padding in a small area");
    formEditor.setVariant(adqt::widgets::AdTextEdit::Variant::Borderless);
    formEditor.setHeightMode(adqt::widgets::AdTextEdit::HeightMode::FixedRows);
    QApplication::processEvents();
    require(formEditor.viewport()->geometry().left() > 0,
            "a borderless form editor sized by rows must retain its padding");
}

void formattedClipboardTextUsesASelectableQtDocument() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QWidget host;
    host.resize(360, 160);
    host.show();
    const QSizeF canvasSize(host.width() * 2.0, host.height() * 2.0);
    ScreenshotRecognitionWindow window(
        ScreenshotRecognitionWindowActions{}, &host,
        ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
    require(window.present(ScreenshotRecognitionWindow::Config{
                screen,
                &host,
                host.rect(),
                QRectF(QPointF(), canvasSize),
                ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild,
                2.0,
            }),
            "the formatted clipboard recognition surface should present");

    auto document = std::make_shared<QTextDocument>();
    document->setDocumentMargin(0.0);
    document->setHtml(QStringLiteral(
        "<p style=\"font-size: 40px; margin: 0\"><b>Formatted</b> clipboard text</p>"));
    document->setTextWidth(host.width());
    QImage canonicalImage(canvasSize.toSize(), QImage::Format_ARGB32_Premultiplied);
    canonicalImage.setDevicePixelRatio(2.0);
    canonicalImage.fill(Qt::white);
    QPainter canonicalPainter(&canonicalImage);
    canonicalPainter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing |
                                    QPainter::SmoothPixmapTransform);
    document->drawContents(&canonicalPainter);
    canonicalPainter.end();
    window.showFormattedText(document);
    QApplication::processEvents();

    auto* textLayer = window.findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText"));
    auto* textItem = formattedTextItem(textLayer);
    const Qt::TextInteractionFlags selectionFlags =
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard;
    require(textLayer != nullptr && textLayer->isVisible() && textLayer->isInteractive() &&
                textItem != nullptr && textItem->document() == document.get() &&
                (textItem->textInteractionFlags() & selectionFlags) == selectionFlags &&
                !textItem->openExternalLinks(),
            "formatted clipboard text should retain its Qt document and allow direct selection");
    require(textItem->toPlainText().contains(QStringLiteral("Formatted clipboard text")),
            "the selectable clipboard surface should preserve document text");
    require(renderFormattedTextItem(textItem, true) == renderFormattedTextItem(textItem, false),
            "formatted clipboard text should not paint Qt's dashed focus outline");
    require(std::abs(textLayer->transform().m11() - 0.5) < 0.001 &&
                std::abs(textLayer->transform().m22() - 0.5) < 0.001 &&
                qFuzzyCompare(textItem->scale(), 2.0),
            "formatted clipboard text should map canonical image pixels through the canvas "
            "transform using the owning display DPR");

    QPoint requestedContextMenuPosition;
    int contextMenuRequestCount = 0;
    QObject::connect(&window, &ScreenshotRecognitionWindow::embeddedContextMenuRequested, &window,
                     [&](const QPoint& globalPosition) {
                         requestedContextMenuPosition = globalPosition;
                         ++contextMenuRequestCount;
                     });
    const QPoint localContextMenuPosition = textLayer->viewport()->rect().center();
    const QPoint globalContextMenuPosition =
        textLayer->viewport()->mapToGlobal(localContextMenuPosition);
    QContextMenuEvent contextMenuEvent(QContextMenuEvent::Mouse, localContextMenuPosition,
                                       globalContextMenuPosition);
    QApplication::sendEvent(textLayer->viewport(), &contextMenuEvent);
    require(contextMenuRequestCount == 1 &&
                requestedContextMenuPosition == globalContextMenuPosition &&
                contextMenuEvent.isAccepted(),
            "embedded formatted text should route context menus to its owning pinned window");

    textItem->clearFocus();
    textLayer->clearFocus();
    QImage dpiImage(canvasSize.toSize(), QImage::Format_ARGB32_Premultiplied);
    dpiImage.setDevicePixelRatio(2.0);
    dpiImage.fill(Qt::white);
    QPainter dpiPainter(&dpiImage);
    textLayer->render(&dpiPainter, QRectF(QPointF(), QSizeF(textLayer->size())), textLayer->rect());
    dpiPainter.end();
    const QRect canonicalBounds = nonWhitePixelBounds(canonicalImage);
    const QRect dpiBounds = nonWhitePixelBounds(dpiImage);
    require(canonicalBounds.isValid() && dpiBounds.isValid() &&
                std::abs(canonicalBounds.left() - dpiBounds.left()) <= 1 &&
                std::abs(canonicalBounds.top() - dpiBounds.top()) <= 1 &&
                std::abs(canonicalBounds.width() - dpiBounds.width()) <= 2 &&
                std::abs(canonicalBounds.height() - dpiBounds.height()) <= 2,
            "a 2x backing image should reproduce the canonical HTML text size through the "
            "canvas transform");

    window.resize(180, 80);
    QApplication::processEvents();
    require(std::abs(textLayer->transform().m11() - 0.25) < 0.001 &&
                std::abs(textLayer->transform().m22() - 0.25) < 0.001,
            "formatted clipboard text should track subsequent canvas zoom changes");

    auto replacement = std::make_shared<QTextDocument>();
    replacement->setPlainText(QStringLiteral("Replacement formatted text"));
    window.showFormattedText(replacement);
    QApplication::processEvents();
    textLayer = window.findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText"));
    textItem = formattedTextItem(textLayer);
    require(textItem != nullptr && textItem->document() == replacement.get() &&
                qFuzzyCompare(textItem->scale(), 2.0),
            "replacing visible formatted text should detach the previous document first");

    window.clearFormattedText();
    require(window.findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText")) == nullptr,
            "leaving formatted-text mode should remove its selectable surface");
}

void selectionOnlyFormattedTextPaintsOnlyHighlights() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "formatted selection rendering requires a screen");
    for (const qreal dpr : {1.0, 2.0}) {
        QWidget host;
        host.resize(360, 150);
        QPalette palette = host.palette();
        palette.setColor(QPalette::Window, QColor(24, 72, 120));
        host.setPalette(palette);
        host.setAutoFillBackground(true);
        host.show();
        ScreenshotRecognitionWindow window(
            {}, &host, ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
        require(window.present({screen, &host, host.rect(),
                                QRectF(QPointF(), QSizeF(host.size()) * dpr),
                                ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild, dpr}),
                "present the formatted selection-only overlay");
        QApplication::processEvents();
        const auto original = host.grab().toImage();
        auto document = std::make_shared<QTextDocument>();
        QImage inlineImage(20, 20, QImage::Format_ARGB32_Premultiplied);
        inlineImage.fill(Qt::red);
        document->addResource(QTextDocument::ImageResource, QUrl(QStringLiteral("inline-image")),
                              inlineImage);
        document->setHtml(
            QStringLiteral("<p style=\"color: red; background-color: yellow\"><b>Rich</b> "
                           "<u>wrapped</u> selection text <img src=\"inline-image\"></p>"
                           "<table border=\"1\"><tr><td>Cell</td></tr></table>"));
        document->setTextWidth(160);
        const QString originalHtml = document->toHtml();
        window.showFormattedText(document, ScreenshotOcrTextLayer::RenderingMode::SelectionOnly,
                                 false);
        QApplication::processEvents();
        require(host.grab().toImage() == original,
                "hidden formatted text must not repaint text, HTML backgrounds, or table borders");
        auto* layer = window.findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText"));
        auto* item = formattedTextItem(layer);
        require(item && item->document() == document.get(),
                "selection-only rendering must use the original document layout");
        QTextCursor cursor(document.get());
        cursor.select(QTextCursor::Document);
        item->setTextCursor(cursor);
        QApplication::processEvents();
        const auto selected = host.grab().toImage();
        require(selected != original, "formatted selection must paint visible highlights");
        QImage overlay(host.size(), QImage::Format_ARGB32_Premultiplied);
        overlay.fill(Qt::transparent);
        QPainter painter(&overlay);
        layer->render(&painter);
        painter.end();
        int maximumAlpha = 0;
        for (int y = 0; y < overlay.height(); ++y)
            for (int x = 0; x < overlay.width(); ++x)
                maximumAlpha = std::max(maximumAlpha, qAlpha(overlay.pixel(x, y)));
        require(maximumAlpha == 102,
                "formatted selection paints only translucent highlights, without rich text glyphs");
        require(document->toHtml() == originalHtml, "paint policy must not modify the source HTML");
        window.showFormattedText(document, ScreenshotOcrTextLayer::RenderingMode::SelectionOnly,
                                 false);
        require(item->textCursor().hasSelection(), "synchronization preserves native selection");
        window.clearTextSelection();
        QApplication::processEvents();
        require(host.grab().toImage() == original,
                "clearing formatted selection must restore the original pixels");
        window.showFormattedText(document);
        QApplication::processEvents();
        require(host.grab().toImage() != original,
                "displayed formatted text retains normal painting");
    }
}

void qrContentsUseStrictRichTextLinksAndPreserveOrder() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    QWidget overlayHost;
    overlayHost.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    overlayHost.setGeometry(screen->geometry());
    overlayHost.show();

    QList<QUrl> activatedLinks;
    int recognitionCancelCalls = 0;
    int recognitionCopyCalls = 0;
    ScreenshotRecognitionWindowActions actions;
    actions.handleCancel = [&recognitionCancelCalls]() { ++recognitionCancelCalls; };
    actions.handleCopy = [&recognitionCopyCalls]() { ++recognitionCopyCalls; };
    actions.handleLinkActivated = [&activatedLinks](const QUrl& url) {
        activatedLinks.push_back(url);
    };
    snow_shot::presentation::WindowShortcutManager shortcutManager;
    shortcutManager.addScopeWindow(&overlayHost);
    int sessionShortcutCalls = 0;
    snow_shot::presentation::WindowShortcutManager::Binding sessionShortcut;
    sessionShortcut.id = QStringLiteral("test.qr.screenshot.command");
    sessionShortcut.keyCombinations = {
        QKeyCombination(Qt::NoModifier, Qt::Key_P),
    };
    sessionShortcut.activate = [&sessionShortcutCalls](const auto&) {
        ++sessionShortcutCalls;
        return true;
    };
    require(shortcutManager.addBinding(&overlayHost, std::move(sessionShortcut)) != 0,
            "QR shortcut regression setup should register the screenshot command binding");
    int lowerPriorityCopyCalls = 0;
    snow_shot::presentation::WindowShortcutManager::Binding lowerPriorityCopy;
    lowerPriorityCopy.id = QStringLiteral("test.qr.screenshot.copy");
    lowerPriorityCopy.keyCombinations = {
        QKeyCombination(Qt::ControlModifier, Qt::Key_C),
    };
    lowerPriorityCopy.priority =
        snow_shot::presentation::WindowShortcutManager::StandardPriority::ScreenshotShortcut;
    lowerPriorityCopy.activate = [&lowerPriorityCopyCalls](const auto&) {
        ++lowerPriorityCopyCalls;
        return true;
    };
    require(shortcutManager.addBinding(&overlayHost, std::move(lowerPriorityCopy)) != 0,
            "QR shortcut regression setup should register the screenshot copy binding");
    ScreenshotRecognitionWindow window(
        std::move(actions), nullptr, ScreenshotRecognitionWindow::PresentationMode::TopLevelWindow,
        &shortcutManager);
    const QRect geometry(screen->availableGeometry().center() - QPoint(240, 90), QSize(480, 180));
    require(window.present(ScreenshotRecognitionWindow::Config{
                screen,
                &overlayHost,
                geometry,
                QRectF(0.0, 0.0, 480.0, 180.0),
            }),
            "the QR recognition window should present a valid centered geometry");

    const QStringList contents{
        QStringLiteral("first <b>& payload"),
        QStringLiteral("https://example.com/path?x=1&y=2"),
        QStringLiteral("ftp://example.com/not-clickable"),
        QStringLiteral("prefix https://example.com/not-complete"),
        QStringLiteral("  http://qt.io/docs  "),
    };
    window.showQrContents(contents);
    QApplication::processEvents();

    auto* browser = window.findChild<QTextBrowser*>(QStringLiteral("screenshotQrContents"));
    require(browser != nullptr && browser->isVisible() && browser->isReadOnly() &&
                !browser->openExternalLinks() && !browser->openLinks(),
            "QR contents should use a read-only browser with controller-owned navigation");
    require(browser->toPlainText() == contents.join(QLatin1Char('\n')),
            "QR payloads should be rendered as escaped text in recognition order");
    require(QApplication::focusWidget() == browser,
            "QR results should focus their read-only selection surface");
    const QTextCursor presentedSelection = browser->textCursor();
    require(presentedSelection.hasSelection() &&
                presentedSelection.selectedText() == contents.join(QChar(0x2029)),
            "QR results should present every payload selected for a plain copy");

    PhysicalKeyEvent sessionShortcutEvent(QEvent::KeyPress, Qt::Key_P, Qt::NoModifier);
    QApplication::sendEvent(browser, &sessionShortcutEvent);
    require(sessionShortcutEvent.isAccepted() && sessionShortcutCalls == 1,
            "screenshot-session shortcuts should dispatch while QR results have focus");

    PhysicalKeyEvent copyAll(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(browser, &copyAll);
    require(copyAll.isAccepted() && lowerPriorityCopyCalls == 0 &&
                QGuiApplication::clipboard()->text() == contents.join(QLatin1Char('\n')) &&
                recognitionCopyCalls == 1,
            "Ctrl+C should copy the presented QR selection and end the screenshot");
    require(qobject_cast<adqt::widgets::AdScrollBar*>(browser->verticalScrollBar()) != nullptr,
            "QR contents should use the themed vertical scrollbar");

    QStringList anchorTexts;
    QStringList anchorHrefs;
    for (QTextBlock block = browser->document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isAnchor()) {
                anchorTexts.push_back(fragment.text());
                anchorHrefs.push_back(fragment.charFormat().anchorHref());
            }
        }
    }
    require(anchorTexts == QStringList{QStringLiteral("https://example.com/path?x=1&y=2"),
                                       QStringLiteral("http://qt.io/docs")},
            "only complete HTTP and HTTPS payloads should become links");
    require(anchorHrefs.size() == 2 &&
                QUrl(anchorHrefs.at(0)).host() == QStringLiteral("example.com") &&
                QUrl(anchorHrefs.at(1)).host() == QStringLiteral("qt.io"),
            "recognized links should preserve valid absolute navigation targets");

    const QUrl clicked(QStringLiteral("https://example.com/path?x=1&y=2"));
    require(QMetaObject::invokeMethod(browser, "anchorClicked", Qt::DirectConnection,
                                      Q_ARG(QUrl, clicked)),
            "the QR browser should expose its anchor activation signal");
    require(activatedLinks == QList<QUrl>{clicked},
            "QR anchor activation should be forwarded exactly once to the controller");

    window.showQrContents({});
    QApplication::clipboard()->setText(QStringLiteral("stale clipboard text"));
    PhysicalKeyEvent copyEmpty(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(browser, &copyEmpty);
    require(copyEmpty.isAccepted() && QApplication::clipboard()->text().isEmpty() &&
                recognitionCopyCalls == 2,
            "Ctrl+C should directly copy empty text for a completed QR result with no payload");

    PhysicalKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(browser, &escape);
    require(escape.isAccepted() && recognitionCancelCalls == 0,
            "Escape press must leave the recognition window open");
    PhysicalKeyEvent escapeRelease(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(browser, &escapeRelease);
    require(escapeRelease.isAccepted() && recognitionCancelCalls == 1,
            "Escape should remain available while QR results have focus");

    window.clearQrContents();
    require(window.findChild<QTextBrowser*>(QStringLiteral("screenshotQrContents")) == nullptr,
            "leaving QR mode should remove its rich-text content surface");
    window.hide();
    QApplication::processEvents();
}

void emptyOcrResultCopiesEmptyText() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "a primary screen is required");

    int recognitionCopyCalls = 0;
    ScreenshotRecognitionWindowActions actions;
    actions.handleCopy = [&recognitionCopyCalls]() { ++recognitionCopyCalls; };
    ScreenshotRecognitionWindow window(std::move(actions));
    require(window.present(ScreenshotRecognitionWindow::Config{
                screen,
                nullptr,
                QRect(screen->availableGeometry().center() - QPoint(120, 60), QSize(240, 120)),
                QRectF(0.0, 0.0, 240.0, 120.0),
            }),
            "the empty OCR copy test should present a valid recognition window");

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(0, 0, 240, 120);
    window.setOcrPresentation(std::move(presentation));
    QApplication::processEvents();
    QApplication::clipboard()->setText(QStringLiteral("stale clipboard text"));

    PhysicalKeyEvent copyEmpty(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(&window, &copyEmpty);
    require(copyEmpty.isAccepted() && QApplication::clipboard()->text().isEmpty() &&
                recognitionCopyCalls == 1,
            "Ctrl+C should directly copy empty text for a completed OCR result with no lines");
}
void originalImageOverrideHidesEveryContentPage() {
    for (const auto mode : {ScreenshotRecognitionWindow::PresentationMode::TopLevelWindow,
                            ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild}) {
        QWidget parent;
        parent.resize(320, 200);
        parent.show();
        ScreenshotRecognitionWindow window({}, &parent, mode);
        window.resize(320, 200);
        window.show();
        auto* content = window.findChild<QWidget*>(QStringLiteral("screenshotRecognitionContent"));
        require(content != nullptr, "recognition has a common content surface");
        const auto check = [&](const std::function<void()>& update) {
            window.setShowOriginalImage(true);
            update();
            QCoreApplication::processEvents();
            require(content->isHidden(),
                    "content updates cannot override original-image visibility");
            for (auto* widget : content->findChildren<QWidget*>()) {
                require(!widget->isVisible(), "all recognition descendants stay hidden");
            }
            window.setShowOriginalImage(false);
            require(content->isVisible(), "disabling original-image mode reveals current page");
        };
        auto presentation = std::make_shared<ScreenshotOcrPresentation>();
        presentation->selection = QRect(0, 0, 320, 200);
        presentation->prepareForRendering();
        check([&]() { window.setOcrPresentation(presentation); });
        QImage image(320, 200, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::blue);
        window.setShowOriginalImage(true);
        require(!window.imageSnapshot(image, image.rect(), {}, {}, {}),
                "hidden recognition is excluded from image snapshots");
        QTextDocument document;
        document.setPlainText(QStringLiteral("Preserved draft"));
        check([&]() {
            window.showTextEditor(&document, true, true);
            document.setPlainText(QStringLiteral("Streaming translation"));
            window.setTextEditorStreaming(false);
        });
        check([&]() { window.showTextEditor(&document); });
        auto* editor = window.findChild<QTextEdit*>();
        window.setShowOriginalImage(true);
        window.setShowOriginalImage(false);
        require(window.findChild<QTextEdit*>() == editor && editor->document() == &document,
                "visibility toggle retains the text editor and document");
        auto formatted = std::make_shared<QTextDocument>();
        formatted->setPlainText(QStringLiteral("Formatted content"));
        check([&]() { window.showFormattedText(formatted); });
        auto table = std::make_shared<ScreenshotTableEditingSession>(
            ScreenshotTableDocument::fromPlainText(QStringLiteral("A\tB\nC\tD")));
        check([&]() { window.setTableSession(table); });
        auto* tableEditor = window.findChild<ScreenshotTableEditor*>();
        require(tableEditor->model()->setData(tableEditor->model()->index(0, 0),
                                              QStringLiteral("Edited cell"), Qt::EditRole),
                "table accepts cell edit");
        QCoreApplication::processEvents();
        window.setShowOriginalImage(true);
        window.setShowOriginalImage(false);
        require(window.findChild<ScreenshotTableEditor*>() == tableEditor,
                "visibility toggle retains the table editor and its session");
        require(table->document.cellText(0, 0) == QStringLiteral("Edited cell"),
                "table edits survive original-image viewing");
        window.undoTableEdit();
        require(table->document.cellText(0, 0) == QStringLiteral("A"),
                "table undo history survives");
        window.redoTableEdit();
        require(table->document.cellText(0, 0) == QStringLiteral("Edited cell"),
                "table redo history survives");
        check([&]() { window.showQrContents({QStringLiteral("QR payload")}); });
        for (const auto format :
             {SnowShotImageConversionFormat::Markdown, SnowShotImageConversionFormat::Html}) {
            check(
                [&]() { window.showImageConversion(format, QStringLiteral("content"), true, {}); });
            check([&]() {
                window.showImageConversion(format, QStringLiteral("completed"), false, {});
            });
        }
    }
}

void imageSnapshotTracksOnlyOriginalImageAndOwnsItsResult() {
    ScreenshotRecognitionWindow window({});
    QImage image(240, 120, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    const QRectF rect(50, 30, 240, 120);
    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = rect.toRect();
    ScreenshotOcrLine line;
    line.text = QStringLiteral("Visible translation");
    line.quad = QPolygonF(QRectF(60, 40, 180, 40));
    line.quad.removeLast();
    presentation->lines.push_back(line);
    window.setOcrPresentation(presentation);
    presentation->selectAll();
    auto snapshot = window.imageSnapshot(image, rect, {}, {}, {});
    require(snapshot && snapshot->lines.size() == 1 && snapshot->canvasRect == rect,
            "original-image surface yields a value snapshot in canvas coordinates");
    presentation->setLineText(0, QStringLiteral("Later streaming result"));
    image.fill(Qt::red);
    require(snapshot->lines[0].text == QStringLiteral("Visible translation") &&
                snapshot->image.pixelColor(0, 0) == QColor(Qt::white),
            "later text/image changes cannot mutate the saved snapshot");
    QTextDocument document;
    document.setPlainText(QStringLiteral("Editor draft"));
    window.showTextEditor(&document);
    require(!window.imageSnapshot(image, rect, {}, {}, {}), "text editor is excluded");
    window.showQrContents({QStringLiteral("https://example.com")});
    require(!window.imageSnapshot(image, rect, {}, {}, {}), "QR surface is excluded");
    auto formatted = std::make_shared<QTextDocument>();
    formatted->setHtml(QStringLiteral("<p>Formatted recognition result</p>"));
    window.showFormattedText(formatted);
    require(!window.imageSnapshot(image, rect, {}, {}, {}), "formatted text surface is excluded");
    window.setTableSession(std::make_shared<ScreenshotTableEditingSession>(
        ScreenshotTableDocument::fromPlainText(QStringLiteral("A\tB\nC\tD"))));
    require(!window.imageSnapshot(image, rect, {}, {}, {}), "table surface is excluded");
    window.setOcrPresentation({});
    snapshot = window.imageSnapshot(image, rect, {}, {}, {});
    require(snapshot && snapshot->lines.isEmpty(),
            "pending OCR snapshots the current image without waiting");
}
void defaultSelectionResizeActionsDeclineInteraction() {
    // Both empty and partial aggregates are used by embedded recognition hosts.
    for (const auto& actions : {ScreenshotRecognitionWindowActions{},
                                ScreenshotRecognitionWindowActions{.handleCancel = []() {}}}) {
        for (const QPointF& point : {QPointF(), QPointF(19.5, -42.0)}) {
            require(actions.selectionResizeDragMode(point) == ScreenshotSelectionDragMode::None,
                    "an unwired recognition surface must expose no resize handle");
            require(!actions.beginSelectionResize(point),
                    "an unwired recognition surface must decline selection resizing");
        }
    }
}

void selectionResizeCompletionCanReplaceWindow() {
    QPointer<ScreenshotRecognitionWindow> window;
    bool finished = false;
    ScreenshotRecognitionWindowActions actions;
    actions.selectionResizeDragMode = [](const QPointF&) {
        return ScreenshotSelectionDragMode::Right;
    };
    actions.beginSelectionResize = [](const QPointF&) { return true; };
    actions.finishSelectionResize = [&](const QPointF&) {
        require(QWidget::mouseGrabber() != window.data(),
                "completion must release capture before deleting the recognition window");
        delete window.data();
    };
    actions.selectionResizeFinished = [&]() { finished = true; };
    window = new ScreenshotRecognitionWindow(std::move(actions));
    require(window->present({QGuiApplication::primaryScreen(), nullptr, QRect(50, 50, 240, 120),
                             QRectF(0, 0, 240, 120)}),
            "completion regression should present the recognition surface");
    const QPointF position(238, 60);
    QMouseEvent press(QEvent::MouseButtonPress, position, position, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(window.data(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, position, position, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(window.data(), &release);
    require(window.isNull() && finished,
            "resize completion must finish safely when its callback deletes the window");
}

void selectionResizeKeepsMouseCaptureWhenContentIsCleared() {
    for (const bool table : {false, true}) {
        for (const bool acceptResize : {false, true}) {
            ScreenshotRecognitionWindow* surface = nullptr;
            int updates = 0;
            int finishes = 0;
            ScreenshotRecognitionWindowActions actions;
            actions.selectionResizeDragMode = [](const QPointF&) {
                return ScreenshotSelectionDragMode::Right;
            };
            actions.beginSelectionResize = [&](const QPointF&) {
                require(
                    QWidget::mouseGrabber() == surface,
                    "resize must own mouse capture before recognition clears the pressed child");
                if (acceptResize) {
                    surface->clearTableSession();
                    surface->clearQrContents();
                }
                return acceptResize;
            };
            actions.updateSelectionResize = [&](const QPointF&) { ++updates; };
            actions.finishSelectionResize = [&](const QPointF&) {
                require(QWidget::mouseGrabber() != surface,
                        "resize must release capture before completion can replace its window");
                ++finishes;
            };
            ScreenshotRecognitionWindow window(std::move(actions));
            surface = &window;
            require(window.present({QGuiApplication::primaryScreen(), nullptr,
                                    QRect(50, 50, 240, 120), QRectF(0, 0, 240, 120)}),
                    "resize regression should present the recognition surface");
            QWidget* viewport = nullptr;
            if (table) {
                window.setTableSession(std::make_shared<ScreenshotTableEditingSession>(
                    ScreenshotTableDocument::fromPlainText(QStringLiteral("A\tB\nC\tD"))));
                viewport = window.findChild<ScreenshotTableEditor*>()->viewport();
            } else {
                window.showQrContents({QStringLiteral("https://example.com")});
                viewport = window.findChild<QTextBrowser*>()->viewport();
            }
            QApplication::processEvents();
            const QPointF position(2, 2);
            QMouseEvent press(QEvent::MouseButtonPress, position,
                              QPointF(viewport->mapToGlobal(position.toPoint())), Qt::LeftButton,
                              Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(viewport, &press);
            require((QWidget::mouseGrabber() == &window) == acceptResize,
                    "only an accepted resize should retain mouse capture");
            QMouseEvent move(QEvent::MouseMove, position, position, Qt::NoButton, Qt::LeftButton,
                             Qt::NoModifier);
            QApplication::sendEvent(&window, &move);
            require(updates == (acceptResize ? 1 : 0),
                    "accepted resize must update while the button is held");
            QMouseEvent release(QEvent::MouseButtonRelease, position, position, Qt::LeftButton,
                                Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(&window, &release);
            require(finishes == (acceptResize ? 1 : 0) && QWidget::mouseGrabber() != &window,
                    "release must finish the resize and relinquish mouse capture");
            QMouseEvent hover(QEvent::MouseMove, position, position, Qt::NoButton, Qt::NoButton,
                              Qt::NoModifier);
            QApplication::sendEvent(&window, &hover);
            require(updates == (acceptResize ? 1 : 0),
                    "mouse movement after release must not continue resizing");
        }
    }
}

void translationCopyProviderHandlesKeyboardAndContextMenu() {
    int completed = 0;
    bool fail = false;
    QTextDocument document;
    document.setPlainText(QStringLiteral("Full translated result\n").repeated(50));
    ScreenshotRecognitionWindowActions actions;
    actions.handleCopy = [&] { ++completed; };
    ScreenshotRecognitionWindow window(std::move(actions));
    auto* screen = QGuiApplication::primaryScreen();
    require(screen &&
                window.present({screen, nullptr, QRect(20, 20, 360, 150), QRectF(0, 0, 360, 150)}),
            "present translation copy window");
    window.showTextEditor(&document, true, true);
    QApplication::processEvents();
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor, "translation copy owns a result editor");
    window.setTranslationClipboardProvider(
        [] { return true; },
        [&]() -> std::unique_ptr<QMimeData> {
            if (fail)
                return {};
            if (auto selected = window.textSelectionClipboardMimeData())
                return selected;
            auto mime = std::make_unique<QMimeData>();
            mime->setImageData(window.translationDocumentImage());
            return mime;
        });
    const auto image = window.translationDocumentImage();
    require(!image.isNull() && image.height() > editor->viewport()->height(),
            "translation image includes content below the viewport");
    editor->selectAll();
    editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->maximum());
    require(window.translationDocumentImage() == image,
            "document rendering excludes selection, caret and scrolling decorations");
    auto cursor = editor->textCursor();
    cursor.clearSelection();
    editor->setTextCursor(cursor);
    inspectContextMenu(*editor->viewport(), editor->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           auto* copy = contextMenuAction(menu, QStringLiteral("Copy"));
                           require(copy->isEnabled(),
                                   "unselected translation enables context Copy");
                           copy->trigger();
                       });
    require(QApplication::clipboard()->mimeData()->hasImage() && completed == 0,
            "context Copy publishes the image without completing the host command");
    window.activateWindow();
    editor->setFocus();
    QApplication::processEvents();
    require(QApplication::focusWidget() == editor, "keyboard Copy focuses the translation editor");
    PhysicalKeyEvent copy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(editor, &copy);
    PhysicalKeyEvent copyRelease(QEvent::KeyRelease, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(editor, &copyRelease);
    require(QApplication::clipboard()->mimeData()->hasImage() && completed == 1,
            "keyboard Copy publishes the image and completes the host command");
    fail = true;
    QApplication::clipboard()->setText(QStringLiteral("keep previous clipboard"));
    PhysicalKeyEvent failedCopy(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier);
    QApplication::sendEvent(editor, &failedCopy);
    require(QApplication::clipboard()->text() == QStringLiteral("keep previous clipboard") &&
                completed == 1,
            "failed image Copy preserves the clipboard without completing or closing the host");
}

void recognitionContextMenusUseAntDesignAndCopyLocally() {
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "context menu tests require a primary screen");

    int copyCommandCalls = 0;
    ScreenshotRecognitionWindowActions actions;
    actions.handleCopy = [&copyCommandCalls]() { ++copyCommandCalls; };
    ScreenshotRecognitionWindow window(std::move(actions));
    window.setOcrCopyDefaultsEnabled(false);
    require(window.present(
                {screen, nullptr,
                 QRect(screen->availableGeometry().center() - QPoint(180, 120), QSize(360, 240)),
                 QRectF(0.0, 0.0, 360.0, 240.0)}),
            "context menu recognition window should present");

    auto presentation = std::make_shared<ScreenshotOcrPresentation>();
    presentation->selection = QRect(0, 0, 360, 240);
    presentation->lines = {
        ScreenshotOcrLine{QStringLiteral("Alpha"), 1.0, QPolygonF(QRectF(20, 30, 140, 40))},
        ScreenshotOcrLine{QStringLiteral("Beta"), 1.0, QPolygonF(QRectF(20, 90, 140, 40))},
    };
    presentation->prepareForRendering();
    window.setOcrPresentation(presentation);
    QApplication::clipboard()->setText(QStringLiteral("stale"));
    inspectContextMenu(window, window.rect().center(), [](adqt::widgets::AdContextMenu& menu) {
        require(menu.objectName() == QStringLiteral("screenshotOcrContextMenu") &&
                    contextMenuActionTexts(menu) ==
                        QStringList{QStringLiteral("Copy"), QStringLiteral("Select All")},
                "OCR context menu should expose Copy and Select All");
        requireContextMenuShortcutsHidden(menu);
        contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
    });
    require(QApplication::clipboard()->text() == QStringLiteral("Alpha\nBeta") &&
                copyCommandCalls == 0 && window.isVisible(),
            "OCR context Copy should copy all text locally without ending capture");

    presentation->beginTextSelection(ScreenshotOcrTextPosition{0, 0});
    presentation->updateTextSelection(ScreenshotOcrTextPosition{0, 5});
    presentation->finishTextSelection();
    window.updateOcrSelection();
    inspectContextMenu(window, window.rect().center(), [](adqt::widgets::AdContextMenu& menu) {
        contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
    });
    require(QApplication::clipboard()->text() == QStringLiteral("Alpha") && copyCommandCalls == 0,
            "OCR context Copy should prefer the current text selection");
    presentation->clearTextSelection();
    window.updateOcrSelection();
    inspectContextMenu(window, window.rect().center(), [](adqt::widgets::AdContextMenu& menu) {
        contextMenuAction(menu, QStringLiteral("Select All"))->trigger();
    });
    require(presentation->selectedText() == QStringLiteral("Alpha\nBeta"),
            "OCR context Select All should update the presentation selection");

    window.showQrContents({QStringLiteral("first"), QStringLiteral("second")});
    QApplication::processEvents();
    auto* qr = window.findChild<QTextBrowser*>(QStringLiteral("screenshotQrContents"));
    require(qr != nullptr, "QR context menu test should expose its browser");
    QTextCursor browserSelection(qr->document());
    browserSelection.setPosition(0);
    browserSelection.setPosition(5, QTextCursor::KeepAnchor);
    qr->setTextCursor(browserSelection);
    inspectContextMenu(
        *qr->viewport(), qr->viewport()->rect().center(), [](adqt::widgets::AdContextMenu& menu) {
            require(menu.objectName() == QStringLiteral("screenshotQrContextMenu") &&
                        contextMenuActionTexts(menu) ==
                            QStringList{QStringLiteral("Copy"), QStringLiteral("Select All")},
                    "QR context menu should expose Copy and Select All");
            requireContextMenuShortcutsHidden(menu);
            contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
        });
    require(QApplication::clipboard()->text() == QStringLiteral("first") && copyCommandCalls == 0,
            "QR context Copy should copy the browser selection locally");
    browserSelection.clearSelection();
    qr->setTextCursor(browserSelection);
    inspectContextMenu(*qr->viewport(), qr->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
                       });
    require(QApplication::clipboard()->text() == QStringLiteral("first\nsecond"),
            "QR context Copy should fall back to all displayed payloads");
    inspectContextMenu(*qr->viewport(), qr->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           contextMenuAction(menu, QStringLiteral("Select All"))->trigger();
                       });
    require(qr->textCursor().hasSelection() && qr->textCursor().selectionStart() == 0 &&
                qr->textCursor().selectionEnd() == qr->document()->characterCount() - 1,
            "QR context Select All should select the complete browser document");

    const QString markdown = QStringLiteral("# Heading\n\nBody");
    window.showImageConversion(SnowShotImageConversionFormat::Markdown, markdown, false, {});
    QApplication::processEvents();
    auto* preview =
        window.findChild<QTextBrowser*>(QStringLiteral("screenshotImageConversionPreview"));
    require(preview != nullptr, "conversion context menu test should expose its preview");
    browserSelection = QTextCursor(preview->document());
    browserSelection.setPosition(0);
    browserSelection.setPosition(7, QTextCursor::KeepAnchor);
    preview->setTextCursor(browserSelection);
    inspectContextMenu(
        *preview->viewport(), preview->viewport()->rect().center(),
        [](adqt::widgets::AdContextMenu& menu) {
            require(menu.objectName() == QStringLiteral("screenshotImageConversionContextMenu") &&
                        contextMenuActionTexts(menu) ==
                            QStringList{QStringLiteral("Copy"), QStringLiteral("Select All")},
                    "conversion context menu should use Ant Design actions");
            requireContextMenuShortcutsHidden(menu);
            contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
        });
    require(QApplication::clipboard()->text() == QStringLiteral("Heading") && copyCommandCalls == 0,
            "conversion context Copy should prefer selected rendered text");
    browserSelection.clearSelection();
    preview->setTextCursor(browserSelection);
    inspectContextMenu(*preview->viewport(), preview->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
                       });
    require(QApplication::clipboard()->text() == markdown,
            "conversion context Copy should fall back to the full source");
    inspectContextMenu(*preview->viewport(), preview->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           contextMenuAction(menu, QStringLiteral("Select All"))->trigger();
                       });
    require(preview->textCursor().hasSelection() && preview->textCursor().selectionStart() == 0 &&
                preview->textCursor().selectionEnd() == preview->document()->characterCount() - 1,
            "conversion context Select All should select the rendered preview document");

    const QString html = QStringLiteral("<h1>Title</h1><p>HTML body</p>");
    window.showImageConversion(SnowShotImageConversionFormat::Html, html, false, {});
    QApplication::processEvents();
    browserSelection = QTextCursor(preview->document());
    browserSelection.setPosition(0);
    browserSelection.setPosition(5, QTextCursor::KeepAnchor);
    preview->setTextCursor(browserSelection);
    inspectContextMenu(*preview->viewport(), preview->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           require(menu.objectName() ==
                                       QStringLiteral("screenshotImageConversionContextMenu"),
                                   "HTML preview should use the conversion Ant Design menu");
                           contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
                       });
    require(QApplication::clipboard()->text() == QStringLiteral("Title"),
            "HTML context Copy should prefer selected rendered text");
    browserSelection.clearSelection();
    preview->setTextCursor(browserSelection);
    inspectContextMenu(*preview->viewport(), preview->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
                       });
    require(QApplication::clipboard()->text() == html,
            "HTML context Copy should fall back to the complete HTML source");
    window.showImageConversion(SnowShotImageConversionFormat::Html, {}, false,
                               QStringLiteral("conversion failed"));
    QApplication::processEvents();
    inspectContextMenu(*preview->viewport(), preview->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           require(!contextMenuAction(menu, QStringLiteral("Copy"))->isEnabled(),
                                   "empty conversion results should disable context Copy");
                       });

    QTextDocument editable;
    editable.setPlainText(QStringLiteral("alpha beta"));
    window.showTextEditor(&editable);
    QApplication::processEvents();
    auto* textEditor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(textEditor != nullptr, "text editor context menu test should expose its editor");
    const auto selectEditorText = [textEditor](int start, int end) {
        QTextCursor cursor(textEditor->document());
        cursor.setPosition(start);
        cursor.setPosition(end, QTextCursor::KeepAnchor);
        textEditor->setTextCursor(cursor);
    };
    QApplication::clipboard()->setText(QStringLiteral("paste"));
    selectEditorText(0, 5);
    inspectContextMenu(
        *textEditor->viewport(), textEditor->viewport()->rect().center(),
        [](adqt::widgets::AdContextMenu& menu) {
            require(menu.objectName() == QStringLiteral("screenshotTextEditorContextMenu") &&
                        contextMenuActionTexts(menu) ==
                            QStringList{QStringLiteral("Copy"), QStringLiteral("Cut"),
                                        QStringLiteral("Paste"), QStringLiteral("Delete"),
                                        QStringLiteral("|"), QStringLiteral("Select All")},
                    "text editor menu should expose all requested actions");
            requireContextMenuShortcutsHidden(menu);
            require(contextMenuAction(menu, QStringLiteral("Copy"))->isEnabled() &&
                        contextMenuAction(menu, QStringLiteral("Cut"))->isEnabled() &&
                        contextMenuAction(menu, QStringLiteral("Paste"))->isEnabled() &&
                        contextMenuAction(menu, QStringLiteral("Delete"))->isEnabled(),
                    "editable selected text should enable edit commands");
            contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
        });
    require(QApplication::clipboard()->text() == QStringLiteral("alpha") && copyCommandCalls == 0,
            "text editor context Copy should remain local");

    selectEditorText(0, 5);
    inspectContextMenu(*textEditor->viewport(), textEditor->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           contextMenuAction(menu, QStringLiteral("Cut"))->trigger();
                       });
    require(editable.toPlainText() == QStringLiteral(" beta"),
            "text editor context Cut should remove selected text");
    QApplication::clipboard()->setText(QStringLiteral("!"));
    QTextCursor editorCursor(textEditor->document());
    editorCursor.movePosition(QTextCursor::End);
    textEditor->setTextCursor(editorCursor);
    inspectContextMenu(*textEditor->viewport(), textEditor->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           require(
                               !contextMenuAction(menu, QStringLiteral("Copy"))->isEnabled() &&
                                   !contextMenuAction(menu, QStringLiteral("Cut"))->isEnabled() &&
                                   contextMenuAction(menu, QStringLiteral("Paste"))->isEnabled() &&
                                   !contextMenuAction(menu, QStringLiteral("Delete"))->isEnabled(),
                               "editable text without a selection should enable only Paste");
                           contextMenuAction(menu, QStringLiteral("Paste"))->trigger();
                       });
    require(editable.toPlainText() == QStringLiteral(" beta!"),
            "text editor context Paste should insert clipboard text");
    selectEditorText(1, 5);
    inspectContextMenu(*textEditor->viewport(), textEditor->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           contextMenuAction(menu, QStringLiteral("Delete"))->trigger();
                       });
    require(editable.toPlainText() == QStringLiteral(" !"),
            "text editor context Delete should remove selected text");
    inspectContextMenu(*textEditor->viewport(), textEditor->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           contextMenuAction(menu, QStringLiteral("Select All"))->trigger();
                       });
    require(textEditor->textCursor().selectedText() == QStringLiteral(" !"),
            "text editor context Select All should select the document");
    window.setTextEditorStreaming(true);
    inspectContextMenu(
        *textEditor->viewport(), textEditor->viewport()->rect().center(),
        [](adqt::widgets::AdContextMenu& menu) {
            require(contextMenuAction(menu, QStringLiteral("Copy"))->isEnabled() &&
                        !contextMenuAction(menu, QStringLiteral("Cut"))->isEnabled() &&
                        !contextMenuAction(menu, QStringLiteral("Paste"))->isEnabled() &&
                        !contextMenuAction(menu, QStringLiteral("Delete"))->isEnabled() &&
                        contextMenuAction(menu, QStringLiteral("Select All"))->isEnabled(),
                    "read-only translation should expose only Copy and Select All");
        });

    auto tableSession = std::make_shared<ScreenshotTableEditingSession>(
        ScreenshotTableDocument::fromPlainText(QStringLiteral("Alpha cell\tB\nC\tD")));
    window.setTableSession(tableSession);
    QApplication::processEvents();
    auto* table =
        window.findChild<ScreenshotTableEditor*>(QStringLiteral("snowShotRecognizedTable"));
    require(table != nullptr, "table context menu test should expose its editor");
    const QModelIndex firstCell = table->model()->index(0, 0);
    table->selectionModel()->select(table->model()->index(0, 1),
                                    QItemSelectionModel::ClearAndSelect);
    inspectContextMenu(
        *table->viewport(), table->visualRect(firstCell).center(),
        [](adqt::widgets::AdContextMenu& menu) {
            require(menu.objectName() == QStringLiteral("screenshotTableContextMenu") &&
                        contextMenuActionTexts(menu) ==
                            QStringList{QStringLiteral("Copy"), QStringLiteral("Paste"),
                                        QStringLiteral("Clear contents"), QStringLiteral("|"),
                                        QStringLiteral("Merge cells"),
                                        QStringLiteral("Split cells")},
                    "table menu should remove Edit and retain table commands");
            requireContextMenuShortcutsHidden(menu);
            contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
        });
    require(QApplication::clipboard()->text() == QStringLiteral("Alpha cell") &&
                copyCommandCalls == 0,
            "table context Copy should copy only selected cells locally");

    clickCell(*table, 0, 0);
    auto* cellEditor = table->findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
    require(cellEditor != nullptr, "inline table editor should open for context menu test");
    QTextCursor cellSelection(cellEditor->document());
    cellSelection.setPosition(0);
    cellSelection.setPosition(5, QTextCursor::KeepAnchor);
    cellEditor->setTextCursor(cellSelection);
    inspectContextMenu(
        *cellEditor->viewport(), cellEditor->viewport()->rect().center(),
        [](adqt::widgets::AdContextMenu& menu) {
            require(menu.objectName() == QStringLiteral("screenshotTableCellEditorContextMenu") &&
                        contextMenuActionTexts(menu) ==
                            QStringList{QStringLiteral("Copy"), QStringLiteral("Cut"),
                                        QStringLiteral("Paste"), QStringLiteral("Delete"),
                                        QStringLiteral("|"), QStringLiteral("Select All")},
                    "inline table text should use the OCR-style Ant Design edit menu");
            requireContextMenuShortcutsHidden(menu);
            require(contextMenuAction(menu, QStringLiteral("Copy"))->isEnabled() &&
                        contextMenuAction(menu, QStringLiteral("Cut"))->isEnabled() &&
                        contextMenuAction(menu, QStringLiteral("Paste"))->isEnabled() &&
                        contextMenuAction(menu, QStringLiteral("Delete"))->isEnabled() &&
                        contextMenuAction(menu, QStringLiteral("Select All"))->isEnabled(),
                    "selected inline table text should enable every edit command");
            contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
        });
    require(QApplication::clipboard()->text() == QStringLiteral("Alpha") &&
                cellEditor->textCursor().selectedText() == QStringLiteral("Alpha") &&
                copyCommandCalls == 0,
            "inline table context Copy should preserve and copy the character selection");
    inspectContextMenu(*cellEditor->viewport(), cellEditor->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           contextMenuAction(menu, QStringLiteral("Cut"))->trigger();
                       });
    require(cellEditor->toPlainText() == QStringLiteral(" cell") &&
                QApplication::clipboard()->text() == QStringLiteral("Alpha"),
            "inline table context Cut should remove and copy the character selection");
    QApplication::clipboard()->setText(QStringLiteral("New"));
    QTextCursor insertionCursor(cellEditor->document());
    insertionCursor.setPosition(0);
    cellEditor->setTextCursor(insertionCursor);
    inspectContextMenu(*cellEditor->viewport(), cellEditor->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           require(
                               !contextMenuAction(menu, QStringLiteral("Copy"))->isEnabled() &&
                                   !contextMenuAction(menu, QStringLiteral("Cut"))->isEnabled() &&
                                   contextMenuAction(menu, QStringLiteral("Paste"))->isEnabled() &&
                                   !contextMenuAction(menu, QStringLiteral("Delete"))->isEnabled(),
                               "inline table text without a selection should enable only Paste");
                           contextMenuAction(menu, QStringLiteral("Paste"))->trigger();
                       });
    require(cellEditor->toPlainText() == QStringLiteral("New cell"),
            "inline table context Paste should insert clipboard text at the cursor");
    QTextCursor deleteCursor(cellEditor->document());
    deleteCursor.setPosition(0);
    deleteCursor.setPosition(3, QTextCursor::KeepAnchor);
    cellEditor->setTextCursor(deleteCursor);
    inspectContextMenu(*cellEditor->viewport(), cellEditor->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           contextMenuAction(menu, QStringLiteral("Delete"))->trigger();
                       });
    require(cellEditor->toPlainText() == QStringLiteral(" cell"),
            "inline table context Delete should remove the character selection");
    inspectContextMenu(*cellEditor->viewport(), cellEditor->viewport()->rect().center(),
                       [](adqt::widgets::AdContextMenu& menu) {
                           contextMenuAction(menu, QStringLiteral("Select All"))->trigger();
                       });
    require(cellEditor->textCursor().selectedText() == QStringLiteral(" cell"),
            "inline table context Select All should select the cell text");
    PhysicalKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(cellEditor, &escape);
    processEditorClose();
    table->selectionModel()->clearSelection();
    inspectContextMenu(*table->viewport(), table->viewport()->rect().bottomRight() - QPoint(2, 2),
                       [](adqt::widgets::AdContextMenu& menu) {
                           require(!contextMenuAction(menu, QStringLiteral("Copy"))->isEnabled(),
                                   "blank table context should not copy the whole table");
                       });

    QWidget embeddedHost;
    embeddedHost.resize(360, 240);
    embeddedHost.show();
    ScreenshotRecognitionWindow embedded(
        {}, &embeddedHost, ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild);
    require(embedded.present({screen, &embeddedHost, embeddedHost.rect(),
                              QRectF(0.0, 0.0, 360.0, 240.0),
                              ScreenshotRecognitionWindow::PresentationMode::EmbeddedChild}),
            "embedded context menu recognition window should present");
    int forwardedMenus = 0;
    QObject::connect(&embedded, &ScreenshotRecognitionWindow::embeddedContextMenuRequested,
                     &embedded, [&forwardedMenus](const QPoint&) { ++forwardedMenus; });
    embedded.setOcrPresentation(presentation);
    inspectContextMenu(embedded, embedded.rect().center(), [](adqt::widgets::AdContextMenu& menu) {
        contextMenuAction(menu, QStringLiteral("Copy"))->trigger();
    });
    require(forwardedMenus == 0, "embedded OCR should own its recognition context menu");
    auto formatted = std::make_shared<QTextDocument>();
    formatted->setPlainText(QStringLiteral("formatted clipboard text"));
    embedded.showFormattedText(formatted);
    auto* formattedLayer =
        embedded.findChild<QGraphicsView*>(QStringLiteral("screenshotClipboardText"));
    require(formattedLayer != nullptr, "embedded fallback should expose formatted text layer");
    const QPoint fallbackPosition = formattedLayer->viewport()->rect().center();
    QContextMenuEvent fallbackEvent(QContextMenuEvent::Mouse, fallbackPosition,
                                    formattedLayer->viewport()->mapToGlobal(fallbackPosition));
    QApplication::sendEvent(formattedLayer->viewport(), &fallbackEvent);
    require(fallbackEvent.isAccepted() && forwardedMenus == 1,
            "unsupported embedded content should still forward to the pinned menu");

    auto embeddedTableSession = std::make_shared<ScreenshotTableEditingSession>(
        ScreenshotTableDocument::fromPlainText(QStringLiteral("Pinned cell\tB\nC\tD")));
    embedded.setTableSession(embeddedTableSession);
    QApplication::processEvents();
    auto* embeddedTable =
        embedded.findChild<ScreenshotTableEditor*>(QStringLiteral("snowShotRecognizedTable"));
    require(embeddedTable != nullptr, "embedded context menu test should expose its table");
    clickCell(*embeddedTable, 0, 0);
    auto* embeddedCellEditor =
        embeddedTable->findChild<QPlainTextEdit*>(QStringLiteral("snowShotTableCellEditor"));
    require(embeddedCellEditor != nullptr,
            "embedded context menu test should open the inline table editor");
    inspectContextMenu(
        *embeddedCellEditor->viewport(), embeddedCellEditor->viewport()->rect().center(),
        [](adqt::widgets::AdContextMenu& menu) {
            require(menu.objectName() == QStringLiteral("screenshotTableCellEditorContextMenu"),
                    "embedded inline table text should own its OCR-style edit menu");
        });
    require(forwardedMenus == 1,
            "embedded inline table editing should not forward to the pinned image menu");
}

void tableClipboardPreservesLargeValuesForWholeTableAndSelection() {
    const QString value = QStringLiteral("251231312312321321321312312321");
    auto session =
        std::make_shared<ScreenshotTableEditingSession>(ScreenshotTableDocument::fromPlainText(
            QStringLiteral("Value\tOther\n") + value + QStringLiteral("\t42")));
    ScreenshotTableEditor editor;
    editor.setSession(session);
    // The OS clipboard is a shared resource: after the rapid writes of the
    // earlier tests the clipboard history service can hold it briefly
    // (CLIPBRD_E_CANT_OPEN), so publishing needs bounded retries before the
    // payload can be judged.
    const auto publishAndRead = [&editor]() -> const QMimeData* {
        for (int attempt = 0; attempt < 40; ++attempt) {
            const bool issued = editor.copySelectionToClipboard();
            QCoreApplication::processEvents();
            const QMimeData* mime = QApplication::clipboard()->mimeData();
            if (issued && mime != nullptr && mime->hasHtml() && mime->hasText()) {
                return mime;
            }
            QThread::msleep(50);
        }
        return nullptr;
    };
    const QMimeData* mime = publishAndRead();
    require(mime != nullptr, "whole-table copy should publish its clipboard payload");
    require(mime->hasHtml() && mime->hasText() &&
                mime->text() ==
                    QStringLiteral("Value\tOther\n'") + value + QStringLiteral("\t42") &&
                ScreenshotTableDocument::fromHtml(mime->html()).toPlainText() == mime->text() &&
                ScreenshotTableDocument::fromClipboardMimeData(*mime) == session->document,
            "whole-table copy should publish the exact text and spreadsheet HTML payload");

    const QModelIndex index = editor.model()->index(1, 0);
    editor.selectionModel()->select(index, QItemSelectionModel::ClearAndSelect);
    mime = publishAndRead();
    require(mime != nullptr, "selected-cell copy should publish its clipboard payload");
    require(mime->text() == QLatin1Char('\'') + value &&
                ScreenshotTableDocument::fromHtml(mime->html()).cellText(0, 0) == mime->text(),
            "selected-cell copy should retain every digit and the spreadsheet HTML payload");
    editor.setCurrentIndex(editor.model()->index(1, 1));
    editor.selectionModel()->clearSelection();
    editor.pasteSelection();
    require(session->document.cellText(1, 1) == value,
            "pasting Snow Shot's protected clipboard should restore the unprefixed value");
}
} // namespace

void tableTabNavigationPreservesDirectionAndFocus() {
    QWidget window;
    QVBoxLayout layout(&window);
    QLineEdit before;
    ScreenshotTableEditor editor;
    QLineEdit after;
    layout.addWidget(&before);
    layout.addWidget(&editor);
    layout.addWidget(&after);
    auto session = std::make_shared<ScreenshotTableEditingSession>(
        ScreenshotTableDocument::fromPlainText(QStringLiteral("a\tb\tc")));
    editor.setSession(session);
    window.show();
    window.activateWindow();
    QApplication::processEvents();
    for (bool reverse : {false, true}) {
        const auto modifiers = reverse ? Qt::ShiftModifier : Qt::NoModifier;
        const auto key = reverse ? Qt::Key_Backtab : Qt::Key_Tab;
        editor.setCurrentIndex(editor.model()->index(0, 1));
        editor.setFocus();
        PhysicalKeyEvent navigation(QEvent::KeyPress, key, modifiers);
        QApplication::sendEvent(&editor, &navigation);
        require(editor.currentIndex().column() == (reverse ? 0 : 2) && editor.hasFocus(),
                "Tab and Shift+Tab must navigate cells without moving focus to sibling widgets");
#ifdef Q_OS_MACOS
        // A native Tab with an unknown legend reaches keyPressEvent directly,
        // without QWidget's focus traversal and its synthesized navigation event.
        editor.setCurrentIndex(editor.model()->index(0, 1));
        QKeyEvent physicalTab(QEvent::KeyPress, Qt::Key_unknown, modifiers, 1, 48, 0);
        QApplication::sendEvent(&editor, &physicalTab);
        require(editor.currentIndex().column() == (reverse ? 0 : 2) && editor.hasFocus(),
                "physical Tab must preserve navigation direction independently of its legend");
#endif
    }
    editor.setTabKeyNavigation(false);
    for (bool reverse : {false, true}) {
        editor.setCurrentIndex(editor.model()->index(0, 1));
        editor.setFocus();
        PhysicalKeyEvent navigation(QEvent::KeyPress, reverse ? Qt::Key_Backtab : Qt::Key_Tab,
                                    reverse ? Qt::ShiftModifier : Qt::NoModifier);
        QApplication::sendEvent(&editor, &navigation);
        require(editor.currentIndex().column() == 1 && (reverse ? before : after).hasFocus(),
                "disabling cell Tab navigation must preserve normal forward and reverse focus "
                "traversal");
    }
}

void tableCommandsUsePhysicalKeys() {
#ifdef Q_OS_MACOS
    auto session = std::make_shared<ScreenshotTableEditingSession>(
        ScreenshotTableDocument::fromPlainText(QStringLiteral("original")));
    ScreenshotTableEditor editor;
    editor.setSession(session);
    editor.show();
    const auto send = [&](int logical, quint32 physical, Qt::KeyboardModifiers modifiers) {
        QKeyEvent event(QEvent::KeyPress, logical, modifiers, 1, physical, 0);
        QApplication::sendEvent(&editor, &event);
    };
    send(Qt::Key_A, 3, Qt::ControlModifier);
    require(!editor.commandState().hasSelection,
            "the base table view must not select all from a logical A at physical F");
    send(Qt::Key_Q, 0, Qt::ControlModifier);
    require(editor.commandState().hasSelection, "table Select All must use physical A");
    editor.setCurrentIndex(editor.model()->index(0, 0));
    QApplication::clipboard()->setText(QStringLiteral("replacement"));
    send(Qt::Key_Q, 9, Qt::ControlModifier);
    require(session->document.cellText(0, 0) == QStringLiteral("replacement"),
            "table Paste must use physical V");
    send(Qt::Key_Q, 6, Qt::ControlModifier);
    require(session->document.cellText(0, 0) == QStringLiteral("original"),
            "table Undo must use physical Z");
    send(Qt::Key_Q, 6, Qt::ControlModifier | Qt::ShiftModifier);
    require(session->document.cellText(0, 0) == QStringLiteral("replacement"),
            "table Redo must use physical Shift+Z");
    QApplication::clipboard()->setText(QStringLiteral("sentinel"));
    send(Qt::Key_Q, 8, Qt::ControlModifier);
    require(QApplication::clipboard()->text() == QStringLiteral("replacement"),
            "table Copy must use physical C");
    send(Qt::Key_unknown, 51, Qt::NoModifier);
    require(session->document.cellText(0, 0).isEmpty(),
            "table Delete must accept native input with an unknown logical key");
#endif
}

void originalImagePreviewPreservesSelectionAndOwnerLifecycle() {
    const snow_shot::storage::PinToScreenSettings settings;
    const QString previousAnchor = settings.mouseWheelZoomMode();
    const auto restoreAnchor =
        qScopeGuard([&] { static_cast<void>(settings.setMouseWheelZoomMode(previousAnchor)); });
    require(settings.setMouseWheelZoomMode(QStringLiteral("bottom_right")),
            "configure the ordinary companion wheel anchor");
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "preview fixture requires a screen");
    auto* window = new ScreenshotRecognitionWindow(ScreenshotRecognitionWindowActions{});
    const QRect geometry(screen->geometry().topLeft() + QPoint(110, 120), QSize(83, 24));
    const QRectF selection(100, 50, 166, 48);
    QImage input(166, 48, QImage::Format_ARGB32_Premultiplied);
    input.fill(QColor(18, 72, 156));
    input.setDevicePixelRatio(2.0);
    require(window->findChild<ScreenshotOriginalImagePreviewWindow*>() == nullptr,
            "creating recognition content must not eagerly create a preview");
    window->setOriginalImagePreviewSource(input, selection);
    window->setOriginalImagePreviewEnabled(true);
    require(window->findChild<ScreenshotOriginalImagePreviewWindow*>() == nullptr,
            "enabling a preview on hidden recognition content must not create a window");
    require(window->present({screen, nullptr, geometry, selection}),
            "small selection preview result presents");
    const auto flush = [] {
        for (int i = 0; i < 4; ++i) {
            QApplication::sendPostedEvents();
            QApplication::processEvents();
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    };
    flush();
    QPointer<ScreenshotOriginalImagePreviewWindow> preview =
        window->findChild<ScreenshotOriginalImagePreviewWindow*>();
    const auto requirePreviewVisible = [&](const char* message) {
        flush();
        const auto previews = window->findChildren<ScreenshotOriginalImagePreviewWindow*>();
        require(previews.size() == 1 && previews.front()->isVisible(), message);
        preview = previews.front();
    };
    const auto requirePreviewDestroyed = [&](const char* message) {
        flush();
        require(preview.isNull() &&
                    window->findChild<ScreenshotOriginalImagePreviewWindow*>() == nullptr,
                message);
    };
    require(preview && preview->isWindow() && preview->isVisible() &&
                preview->size() == geometry.size() && !preview->hasFocus(),
            "preview is a separate passive selection-sized window");
    require(preview->grab().toImage().pixelColor(40, 12) == QColor(18, 72, 156),
            "preview displays the input pixels independently of source DPR metadata");
    const QPointer<ScreenshotOriginalImagePreviewWindow> stackingPreview = preview;
    window->syncOriginalImagePreviewStacking(false);
    requirePreviewVisible("changing stacking policy must keep the preview visible");
    window->syncOriginalImagePreviewStacking(true);
    requirePreviewVisible("restoring stacking policy must keep the preview visible");
    require(stackingPreview && stackingPreview == preview,
            "temporary window-flag remapping must not destroy a visible preview");
    QTextDocument document;
    document.setPlainText(QStringLiteral("A long draft that must not expand the selection"));
    window->showTextEditor(&document);
    flush();
    require(preview->isVisible(), "text editing preserves the separate preview");
    companionWheelZoomPreservesRecognition(*preview, *window, document, true);
    window->refreshOriginalImagePreview();
    flush();
    require(preview->size() == geometry.size(),
            "ordinary recognition refresh must preserve the manually selected preview zoom");
    window->setShowOriginalImage(true);
    require(preview->isHidden(), "original-image mode must hide the preview immediately");
    requirePreviewDestroyed("showing the original image destroys its separate preview");
    require(window->geometry() == geometry,
            "showing the original image preserves selection geometry");
    window->setShowOriginalImage(false);
    window->setShowOriginalImage(true);
    requirePreviewDestroyed("a queued preview refresh must respect the original-image mode");
    window->setShowOriginalImage(false);
    requirePreviewVisible("returning to recognition results creates a new preview");
    window->hideTextEditor();
    window->refreshOriginalImagePreview();
    window->setOriginalImagePreviewSuppressed(true);
    require(preview->isHidden(), "interaction suppression must hide the preview immediately");
    requirePreviewDestroyed(
        "interaction suppression destroys the preview despite queued refreshes");
    window->setOriginalImagePreviewEnabled(false);
    window->setOriginalImagePreviewSuppressed(false);
    requirePreviewDestroyed("suppression end cannot create a disabled preview");
    window->setOriginalImagePreviewEnabled(true);
    requirePreviewVisible("reenabling a preview creates a window from its retained input source");
    require(preview->grab().toImage().pixelColor(40, 12) == QColor(18, 72, 156),
            "recreated previews retain the original source pixels");
    window->setOriginalImagePreviewEnabled(false);
    requirePreviewDestroyed("disabling a visible preview destroys its window");
    window->setOriginalImagePreviewEnabled(true);
    requirePreviewVisible("reenabling a visible host creates a preview on demand");
    window->hide();
    require(preview->isHidden(), "hiding results must hide the companion immediately");
    requirePreviewDestroyed("hiding results destroys the companion");
    window->show();
    requirePreviewVisible("showing results creates a new companion");
    window->showMinimized();
    requirePreviewDestroyed("minimizing results destroys the companion");
    window->showNormal();
    requirePreviewVisible("restoring results creates a new companion");
    const QPointer<ScreenshotOriginalImagePreviewWindow> directlyHiddenPreview = preview;
    window->refreshOriginalImagePreview();
    preview->hide();
    requirePreviewVisible("a queued refresh must recreate a directly hidden preview");
    require(directlyHiddenPreview.isNull(),
            "a queued refresh must not reuse a directly hidden preview");
    preview->hide();
    requirePreviewDestroyed("directly hiding a preview automatically destroys it");
    window->refreshOriginalImagePreview();
    requirePreviewVisible("an explicit refresh recreates a directly hidden preview");
    preview->close();
    requirePreviewDestroyed("closing a preview automatically destroys it");
    window->refreshOriginalImagePreview();
    requirePreviewVisible("an explicit refresh recreates a closed preview");
    window->setOriginalImagePreviewSource({}, {});
    requirePreviewDestroyed("clearing the source destroys the preview");
    window->setOriginalImagePreviewSource(input, selection);
    requirePreviewVisible("restoring valid source data creates a new preview");
    window->setOriginalImagePreviewProvider([] { return std::nullopt; }, window);
    requirePreviewDestroyed("an unavailable provider destroys the preview");
    window->setOriginalImagePreviewProvider(
        [&]() -> std::optional<ScreenshotOriginalImagePreviewState> {
            ScreenshotOriginalImagePreviewState state;
            state.image = input;
            state.imageRectInViewport = QRectF(0, 0, std::numeric_limits<qreal>::infinity(), 48);
            state.resultRect = geometry;
            return state;
        },
        window);
    requirePreviewDestroyed("failed presentation must not retain a newly created hidden preview");
    window->setOriginalImagePreviewProvider({}, window);
    requirePreviewVisible("recovering from failed presentation creates a valid preview");
    const QPointer<ScreenshotOriginalImagePreviewWindow> retiredPreview = preview;
    window->setOriginalImagePreviewSuppressed(true);
    window->setOriginalImagePreviewSuppressed(false);
    requirePreviewVisible("ending suppression before deferred deletion creates a fresh preview");
    require(retiredPreview.isNull(), "a retired preview must never be reused by a queued refresh");
    window->refreshOriginalImagePreview();
    delete window;
    require(preview.isNull(), "destroying results destroys the separate preview");
    flush();
}

void latexPreviewUsesPassiveCompanionAndSurvivesEditing() {
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == QStringLiteral("offscreen")) {
        require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >=
                    0,
                "load offscreen recognition font");
        QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
    }
#endif
    auto& themes = adqt::theme::ThemeManager::instance();
    const auto originalTheme = themes.theme();
    const auto restoreTheme = qScopeGuard([&] { themes.setTheme(originalTheme); });
    themes.setPreset(adqt::theme::ThemeScheme::Light, adqt::theme::ThemeDensity::Compact);
    QScreen* screen = QGuiApplication::primaryScreen();
    require(screen != nullptr, "LaTeX preview fixture has a screen");
    // An externally owned Qt document must outlive the editor using it.
    QTextDocument document;
    LatexPreviewTranslator translator;
    ScreenshotRecognitionWindow window(ScreenshotRecognitionWindowActions{});
    const QRect geometry(screen->availableGeometry().topLeft() + QPoint(320, 120), QSize(200, 100));
    require(window.present({screen, nullptr, geometry, QRectF(0, 0, 200, 100)}),
            "LaTeX editor selection presents");
    QImage original(200, 100, QImage::Format_ARGB32_Premultiplied);
    original.fill(Qt::blue);
    window.setOriginalImagePreviewProvider(
        [&]() -> std::optional<ScreenshotOriginalImagePreviewState> {
            ScreenshotOriginalImagePreviewState state;
            state.image = original;
            state.imageRectInViewport = QRectF(0, 0, 200, 100);
            state.resultRect = geometry;
            state.transientOwner = &window;
            state.mouseWheelZoomMode = QStringLiteral("top_left");
            return state;
        },
        &window);
    window.setOriginalImagePreviewEnabled(false);
    document.setPlainText(QStringLiteral("\\frac{a}{b}+\\sqrt{x}"));
    window.showLatexEditor(&document);
    const auto waitFor = [](const std::function<bool()>& predicate) {
        QElapsedTimer timer;
        timer.start();
        do {
            QApplication::processEvents();
            QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            if (predicate())
                return true;
            QThread::msleep(1);
        } while (timer.elapsed() < 10000);
        return false;
    };
    const auto preview = [&]() {
        return window.findChild<ScreenshotOriginalImagePreviewWindow*>();
    };
    require(waitFor([&] {
                return preview() && preview()->isVisible() &&
                       preview()->accessibleDescription().isEmpty();
            }),
            "real MicroTeX preview renders even when text preview is disabled");
    auto* editor = window.findChild<QTextEdit*>(QStringLiteral("screenshotOcrEditor"));
    require(editor && !editor->isReadOnly() && editor->document() == &document,
            "LaTeX uses the shared writable input component");
    require(preview()->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus) &&
                preview()->testAttribute(Qt::WA_ShowWithoutActivating) &&
                preview()->focusPolicy() == Qt::NoFocus,
            "the formula companion declares passive activation and focus on every backend");
    // Offscreen does not implement passive tool-window activation. The Windows backend
    // can verify that showing the companion preserves the real editor's native focus.
    if (QGuiApplication::platformName() == QStringLiteral("windows")) {
        require(
            waitFor([&] { return editor->hasFocus() && QApplication::activeWindow() == &window; }),
            "showing the native formula companion must preserve editor focus");
    }
    require(window.geometry() == geometry && preview()->size() == geometry.size() &&
                preview()->geometry().right() < window.geometry().left() && !preview()->hasFocus(),
            "formula companion preserves selection size, left placement and passive focus");
    companionWheelZoomPreservesRecognition(*preview(), window, document);
    const QImage valid = preview()->grab().toImage();
    require(valid.pixelColor(0, 0) != QColor(Qt::blue),
            "host preview provider contributes geometry rather than original image pixels");
    const QString capturePath = qEnvironmentVariable("SNOW_TEST_LATEX_PREVIEW_OUTPUT");
    const auto saveVisualFixture = [&](const QImage& formulaImage, const QString& path) {
        const QImage editorImage = window.grab().toImage();
        QImage capture(QSize(formulaImage.width() + 16 + editorImage.width(),
                             std::max(formulaImage.height(), editorImage.height())),
                       QImage::Format_ARGB32_Premultiplied);
        capture.fill(Qt::transparent);
        {
            QPainter painter(&capture);
            painter.drawImage(QRect(QPoint(), formulaImage.size()), formulaImage);
            painter.drawImage(QRect(QPoint(formulaImage.width() + 16, 0), editorImage.size()),
                              editorImage);
        }
        require(capture.save(path), "save the optional formula/editor visual fixture");
    };
    if (!capturePath.isEmpty())
        saveVisualFixture(valid, capturePath);
    const QPointer<ScreenshotOriginalImagePreviewWindow> originalImagePreview = preview();
    window.refreshOriginalImagePreview();
    window.setShowOriginalImage(true);
    require(!editor->isVisible(), "original-image mode hides the formula source editor");
    require(waitFor([&] {
                return originalImagePreview && originalImagePreview == preview() &&
                       preview()->isVisible() && preview()->accessibleDescription().isEmpty();
            }),
            "showing the original image preserves the rendered formula companion");
    document.setPlainText(QStringLiteral("x^2"));
    require(waitFor([&] {
                return originalImagePreview && originalImagePreview == preview() &&
                       preview()->isVisible() && preview()->accessibleDescription().isEmpty() &&
                       preview()->grab().toImage() != valid;
            }),
            "source updates rerender the formula while the original image is shown");
    window.setShowOriginalImage(false);
    require(editor->isVisible() && editor->document() == &document &&
                editor->toPlainText() == QStringLiteral("x^2"),
            "returning from the original image preserves the formula draft and editor");
    window.setShowOriginalImage(true);
    document.setPlainText(QStringLiteral("\\frac{a}{b}+\\sqrt{x}"));
    window.showLatexEditor(&document);
    require(waitFor([&] {
                return preview() && preview()->isVisible() &&
                       preview()->accessibleDescription().isEmpty() &&
                       preview()->grab().toImage() == valid;
            }),
            "a formula session renders when original-image mode is already enabled");
    require(!editor->isVisible(), "starting a formula session preserves original-image mode");
    window.setShowOriginalImage(false);
    const QPointer<ScreenshotOriginalImagePreviewWindow> initialPreview = preview();
    editor->moveCursor(QTextCursor::End);
    QKeyEvent type(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier, QStringLiteral("c"));
    QApplication::sendEvent(editor, &type);
    require(document.toPlainText().endsWith(QLatin1Char('c')) &&
                editor->accessibleName() == QStringLiteral("LaTeX formula source"),
            "typing preserves the formula source's accessible name");
    QFocusEvent blur(QEvent::FocusOut, Qt::OtherFocusReason);
    QFocusEvent focus(QEvent::FocusIn, Qt::OtherFocusReason);
    QApplication::sendEvent(editor, &blur);
    require(editor->accessibleName() == QStringLiteral("LaTeX formula source"),
            "losing focus preserves the formula source's accessible name");
    QApplication::sendEvent(editor, &focus);
    require(editor->accessibleName() == QStringLiteral("LaTeX formula source"),
            "gaining focus preserves the formula source's accessible name");
    document.setPlainText(QStringLiteral("\\unknownSnowShotCommand"));
    require(waitFor([&] {
                return preview() && preview()->accessibleDescription().contains(
                                        QStringLiteral("Check the LaTeX source"));
            }),
            "invalid source produces an inline preview error");
    require(initialPreview == preview() && preview()->grab().toImage() != valid,
            "invalid source dims the retained formula in the same companion");
    if (!capturePath.isEmpty()) {
        const QFileInfo output(capturePath);
        saveVisualFixture(
            preview()->grab().toImage(),
            output.dir().filePath(output.completeBaseName() + QStringLiteral("-invalid.png")));
    }
    themes.setPreset(adqt::theme::ThemeScheme::Dark, adqt::theme::ThemeDensity::Compact);
    require(editor->accessibleName() == QStringLiteral("LaTeX formula source"),
            "theme changes preserve the formula source's accessible name");
    const QColor darkBackground = themes.resolveTheme(&window).colorBgContainer;
    const auto retainedFormulaVisible = [&](const QImage& image) {
        int lightPixels = 0;
        // The status occupies the lower footer. Inspect the upper formula area so that
        // the new error text cannot make an old, dark foreground pass this regression.
        for (int y = image.height() / 8; y < image.height() / 2; ++y) {
            for (int x = image.width() / 8; x < image.width() * 7 / 8; ++x) {
                if (qGray(image.pixel(x, y)) > qGray(darkBackground.rgb()) + 40)
                    ++lightPixels;
            }
        }
        return lightPixels > 10;
    };
    require(waitFor([&] {
                if (!preview() || !preview()->accessibleDescription().contains(
                                      QStringLiteral("Check the LaTeX source")))
                    return false;
                const QImage image = preview()->grab().toImage();
                return image.pixelColor(0, 0) == darkBackground && retainedFormulaVisible(image);
            }),
            "a theme change rerenders the retained valid formula while preserving the draft error");
    require(initialPreview == preview() &&
                editor->toPlainText() == QStringLiteral("\\unknownSnowShotCommand"),
            "fallback rendering preserves the companion and editable invalid source");
    document.setPlainText(QStringLiteral("x^2"));
    document.setPlainText(QStringLiteral("\\int_0^1 x\\,dx"));
    require(waitFor([&] { return preview() && preview()->accessibleDescription().isEmpty(); }),
            "latest valid edit clears the error");
    document.setPlainText(QString());
    require(waitFor([&] {
                return preview() && preview()->accessibleDescription() ==
                                        QStringLiteral("No formula to preview");
            }),
            "an empty draft clears the formula without losing the companion");
    require(editor->toPlainText().isEmpty(), "preview failure must not alter the editable source");
    window.setOriginalImagePreviewSuppressed(true);
    require(waitFor([&] { return preview() == nullptr; }),
            "host interaction suppression tears down a formula companion");
    window.setOriginalImagePreviewSuppressed(false);
    require(waitFor([&] { return preview() && preview()->isVisible(); }),
            "ending suppression restores the formula companion");
    document.setPlainText(QStringLiteral("\\sum_{n=0}^{100} n"));
    window.hide();
    require(waitFor([&] { return preview() == nullptr; }),
            "hiding the host discards pending rendering and the companion");
    window.show();
    require(waitFor([&] { return preview() && preview()->accessibleDescription().isEmpty(); }),
            "restoring the host renders its current draft");
    window.showTextEditor(&document);
    require(waitFor([&] { return preview() == nullptr; }),
            "switching back to text editing disables the formula companion");
    window.setOriginalImagePreviewProvider({}, &window);
    window.setOriginalImagePreviewEnabled(true);
    window.showLatexEditor(&document);
    require(waitFor([&] {
                return preview() && preview()->isVisible() &&
                       preview()->accessibleDescription().isEmpty();
            }),
            "formula placement does not require original pixels when text preview is enabled");
    document.setPlainText(QStringLiteral("\\unknownSnowShotCommand"));
    require(waitFor([&] {
                return preview() && preview()->accessibleDescription() ==
                                        QStringLiteral("Unable to preview this formula. Check the "
                                                       "LaTeX source.");
            }),
            "language-change fixture starts with an invalid formula draft");
    QPointer<ScreenshotOriginalImagePreviewWindow> textPreview;
    {
        require(QCoreApplication::installTranslator(&translator),
                "install the focused preview translator");
        const auto uninstallTranslator =
            qScopeGuard([&] { QCoreApplication::removeTranslator(&translator); });
        require(waitFor([&] {
                    return preview() &&
                           editor->accessibleName() ==
                               QStringLiteral("Translated LaTeX formula source") &&
                           preview()->accessibleName() ==
                               QStringLiteral("Translated LaTeX formula preview") &&
                           preview()->accessibleDescription() ==
                               QStringLiteral("Translated Unable to preview this formula. Check "
                                              "the LaTeX source.");
                }),
                "live language changes retranslate formula source, preview name and cached error");
        window.showTextEditor(&document);
        require(editor->accessibleName() == QStringLiteral("Translated Multiline input"),
                "text reuse restores the input component's translated generic source label");
        window.setOriginalImagePreviewSource(original, QRectF(0, 0, 200, 100));
        require(waitFor([&] {
                    return preview() && preview()->isVisible() &&
                           preview()->accessibleName() ==
                               QStringLiteral("Translated Original image preview");
                }),
                "text mode retains its translated original-image companion name");
        textPreview = preview();
    }
    require(waitFor([&] {
                return textPreview && textPreview == preview() &&
                       preview()->accessibleName() == QStringLiteral("Original image preview");
            }),
            "a live text companion refreshes its cached name when the language changes");
    require(document.toPlainText() == QStringLiteral("\\unknownSnowShotCommand"),
            "retranslation and text reuse preserve the editable draft");
}

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QTemporaryDir storageDirectory;
    require(storageDirectory.isValid(), "recognition tests require isolated storage");
    auto& storage = snow_shot::storage::ApplicationStorage::instance();
    require(storage
                .initialize(
                    {storageDirectory.filePath(QStringLiteral("bin")), storageDirectory.path(), 0})
                .success,
            "recognition test storage must initialize");
    const auto shutdown = qScopeGuard([&] { storage.shutdown(); });
    if (application.arguments().contains(QStringLiteral("--native-surface-only"))) {
        recognitionWindowUsesOrdinaryQtWindowBehavior(true);
        recognitionWindowCanExtendBeyondItsDpiScreen();
        ocrTextHighlightingTests();
        recognitionMessageUsesOnlyItsPaintedShadow();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--ocr-box-selection-only"))) {
#ifdef Q_OS_WIN
        require(QFontDatabase::addApplicationFont(QStringLiteral("C:/Windows/Fonts/segoeui.ttf")) >=
                    0,
                "load offscreen box-selection font");
        QApplication::setFont(QFont(QStringLiteral("Segoe UI")));
#endif
        ocrBoxSelectionUsesCurrentRectangleAndCommitsOnRelease();
        ocrBoxSelectionCancelsWithoutTriggeringWindowCommands();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--ocr-highlighting-only"))) {
        ocrTextHighlightingTests();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--text-editor-layout-only"))) {
        textRecognitionEditorsCollapseMarginsInSmallAreas();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--latex-preview-only"))) {
        latexPreviewUsesPassiveCompanionAndSurvivesEditing();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--preview-window-only"))) {
        originalImagePreviewPreservesSelectionAndOwnerLifecycle();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--ocr-double-click-only"))) {
        ocrDoubleClickCopiesOnlyTheClickedBlock();
        ocrDoubleClickPreservesEditorsAndResizeHandles();
        return 0;
    }
    tableTabNavigationPreservesDirectionAndFocus();
    tableCommandsUsePhysicalKeys();
    if (application.arguments().contains(QStringLiteral("--physical-commands-only"))) {
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--original-image-only"))) {
        originalImageOverrideHidesEveryContentPage();
        embeddedRecognitionWindowPreservesParentSurfaceWithVisibleTextLayer();
        shortRecognitionWindowPreservesExactSelectionGeometryAcrossModes();
        imageSnapshotTracksOnlyOriginalImageAndOwnsItsResult();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--message-shadow-only"))) {
        recognitionMessageUsesOnlyItsPaintedShadow();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--table-clipboard-only"))) {
        tableClipboardPreservesLargeValuesForWholeTableAndSelection();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--selection-only-rendering"))) {
        selectionOnlyTextLayerPaintsOnlyHighlights();
        selectionOnlyFormattedTextPaintsOnlyHighlights();
        formattedClipboardTextUsesASelectableQtDocument();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--image-snapshot-only"))) {
        imageSnapshotTracksOnlyOriginalImageAndOwnsItsResult();
        return 0;
    }
    if (application.arguments().contains(QStringLiteral("--context-menu-only"))) {
        translationCopyProviderHandlesKeyboardAndContextMenu();
        recognitionContextMenusUseAntDesignAndCopyLocally();
        return 0;
    }

    ocrHoverUpdatesCursorWithoutClicking();
    formattedTextHoverUpdatesCursorWithoutClicking();
    if (application.arguments().contains(QStringLiteral("--ocr-hover-only"))) {
        return 0;
    }
    defaultSelectionResizeActionsDeclineInteraction();
    selectionResizeKeepsMouseCaptureWhenContentIsCleared();
    selectionResizeCompletionCanReplaceWindow();
    if (application.arguments().contains(QStringLiteral("--selection-resize-only"))) {
        return 0;
    }
    selectionOnlyTextLayerPaintsOnlyHighlights();
    ocrTextHighlightingTests();
    embeddedRecognitionWindowPreservesParentSurfaceWithVisibleTextLayer();
    ocrDoubleClickCopiesOnlyTheClickedBlock();
    ocrDoubleClickPreservesEditorsAndResizeHandles();
    recognitionWindowCanExtendBeyondItsDpiScreen();
    recognitionMessageUsesOnlyItsPaintedShadow();
    recognitionWindowUsesOrdinaryQtWindowBehavior();
    shortRecognitionWindowPreservesExactSelectionGeometryAcrossModes();
    textRecognitionEditorsCollapseMarginsInSmallAreas();
    formattedClipboardTextUsesASelectableQtDocument();
    selectionOnlyFormattedTextPaintsOnlyHighlights();
    qrContentsUseStrictRichTextLinksAndPreserveOrder();
    emptyOcrResultCopiesEmptyText();
    tableClipboardPreservesLargeValuesForWholeTableAndSelection();
    imageSnapshotTracksOnlyOriginalImageAndOwnsItsResult();
    return 0;
}
