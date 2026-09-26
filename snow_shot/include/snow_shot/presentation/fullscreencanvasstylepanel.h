#ifndef SNOW_SHOT_PRESENTATION_FULLSCREENCANVASSTYLEPANEL_H
#define SNOW_SHOT_PRESENTATION_FULLSCREENCANVASSTYLEPANEL_H

#include "snow_shot/presentation/screenshottoolbarmainpanel.h"
#include "snow_draw_engine_qt/snow_canvas_types.h"

#include <memory>

class SnowCanvasWidget;

namespace snow_shot::presentation {

class FullscreenCanvasStylePanel final : public ScreenshotToolbarPanel {
    Q_OBJECT

  public:
    FullscreenCanvasStylePanel(SnowCanvasWidget& canvas, const SnowCanvasStyleDefaults& defaults,
                               QWidget* parent = nullptr);
    ~FullscreenCanvasStylePanel() override;

    void setActiveTool(SnowCanvasTool tool);
    void setLaserActive(bool active);
    void setLaserStyle(const QColor& color, qreal width, int durationMs);
    void dismissPopups();
    void synchronize();

  signals:
    void laserStyleChanged(const QColor& color, qreal width, int durationMs);
    void toolVariantRequested(SnowCanvasTool tool);

  protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation

#endif
