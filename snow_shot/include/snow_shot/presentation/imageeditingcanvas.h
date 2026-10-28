// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "snow_shot/presentation/imageeditingdocument.h"
#include "snow_draw_engine_qt/snow_canvas_widget.h"
#include "snow_draw_engine_qt/snow_canvas_custom_renderer.h"
#include <optional>

namespace snow_shot::presentation {
class ImageEditingCanvas final : public SnowCanvasWidget, private SnowCanvasCustomRenderer {
  public:
    enum class Mode { Pan, Draw, CropRows, CropColumns };
    explicit ImageEditingCanvas(ImageEditingDocument& document, QWidget* parent = nullptr);
    ~ImageEditingCanvas() override;
    void setMode(Mode mode);
    Mode mode() const {
        return m_mode;
    }
    void fit();
    void actualSize();
    double zoom() const {
        return m_zoom;
    }
    QPointF center() const {
        return m_center;
    }
    std::function<void(ImageEditingDocument::CropAxis, int, int)> cropRequested;
    std::function<bool(QWheelEvent*)> styleWheel;
    std::function<void(bool)> historyRequested;

  protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void syncImage();
    void syncCamera();
    QPointF pixelAt(QPointF view) const;
    QRectF cropStrip(int* pixelCount = nullptr) const;
    QRect cropPreviewDamage() const;
    std::uint64_t contentRevision() const override;
    QList<SnowCanvasBaseImageSource> baseImageSources() const override;
    std::optional<SnowCanvasFilterRenderReference> filterRenderReference() const override;
    void renderBeforeCanvas(QPainter&, const SnowCanvasRenderContext&) override;
    void renderOriginalBackground(QPainter&, const SnowCanvasRenderContext&) override;
    void renderAfterCanvas(QPainter&, const SnowCanvasRenderContext&) override;
    ImageEditingDocument& m_document;
    Mode m_mode = Mode::Pan;
    QPointF m_center;
    double m_zoom = 1;
    bool m_fitted = false;
    bool m_panning = false;
    bool m_cropping = false;
    std::optional<QPointF> m_pointerView;
    QPointF m_last;
    QPointF m_firstPixel;
    QPointF m_lastPixel;
};
} // namespace snow_shot::presentation
