#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTLAYER_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTLAYER_H

#include "snow_shot/presentation/screenshotocrpresentation.h"

#include <QColor>
#include <QGraphicsView>
#include <QRect>
#include <QTransform>
#include <QVector>

#include <limits>
#include <memory>
#include <vector>

class QGraphicsScene;
class ScreenshotOcrGraphicsTextItem;

class ScreenshotOcrTextLayer final : public QGraphicsView {
  public:
    explicit ScreenshotOcrTextLayer(QWidget* parent = nullptr);

    void setPresentation(std::shared_ptr<ScreenshotOcrPresentation> presentation,
                         QVector<QColor> foregrounds = {});
    void clearPresentation();
    void synchronize(const QTransform& canvasToViewTransform, const QRect& viewportRect);
    void updateSelection();
    [[nodiscard]] ScreenshotOcrTextPosition textPositionAt(const QPointF& canvasPosition,
                                                           bool useClosestLine) const;

#if defined(SNOW_SHOT_BENCH_INTERNALS)
    [[nodiscard]] quint64 geometrySynchronizationCount() const {
        return m_geometrySynchronizationCount;
    }
#endif

  private:
    struct TextItem {
        int lineIndex = -1;
        ScreenshotOcrGraphicsTextItem* graphicsText = nullptr;
    };

    void rebuildTextItems();
    void synchronizeTextItem(TextItem& item, const QTransform& canvasToViewTransform);

    QGraphicsScene* m_scene = nullptr;
    std::shared_ptr<ScreenshotOcrPresentation> m_presentation;
    QVector<QColor> m_foregrounds;
    std::vector<TextItem> m_textItems;
    QTransform m_canvasToViewTransform;
    QRect m_viewportRect;
    ScreenshotOcrTextPosition m_selectionAnchor;
    ScreenshotOcrTextPosition m_selectionFocus;
    quint64 m_selectionRevision = std::numeric_limits<quint64>::max();
#if defined(SNOW_SHOT_BENCH_INTERNALS)
    quint64 m_geometrySynchronizationCount = 0;
#endif
    bool m_synchronized = false;
};

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTOCRTEXTLAYER_H
