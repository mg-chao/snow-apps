#pragma once

#include "snow_shot/presentation/screenshotpdfexport.h"
#include <QImage>
#include <QPixmap>
#include <QWidget>

class QLabel;
class QTimer;

class ScreenshotSavePreviewCanvas final : public QWidget {
    Q_OBJECT
  public:
    explicit ScreenshotSavePreviewCanvas(QWidget* parent = nullptr);
    void setSource(QImage image, QSize pixels);
    void setOutput(QImage image);
    void setPdfLayout(screenshot_pdf::Layout layout);
    [[nodiscard]] screenshot_pdf::Layout pdfLayout() const {
        return m_pdfLayout;
    }
    [[nodiscard]] const QImage& outputImage() const {
        return m_output;
    }
    void setSplitRatio(double value);
    [[nodiscard]] double splitRatio() const {
        return m_split;
    }
    [[nodiscard]] double zoom() const {
        return m_zoom;
    }
    [[nodiscard]] QPointF pan() const {
        return m_pan;
    }
    void fitImage(bool showReadout = false);
    void setBusy(bool busy);

  protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void changeEvent(QEvent*) override;

  private:
    void updateReadout();
    void showReadout();
    void zoomAt(double value, QPointF position);
    [[nodiscard]] bool splitHandleContains(QPointF position) const;
    screenshot_pdf::Layout m_pdfLayout;
    QImage m_original;
    QImage m_output;
    QPixmap m_checkerboard;
    QSize m_sourcePixels;
    QPointF m_pan;
    QPointF m_last;
    double m_split = 0.5;
    double m_zoom = 1;
    bool m_dragging = false;
    bool m_splitDragging = false;
    bool m_splitHovered = false;
    bool m_fit = true;
    QLabel* m_readout = nullptr;
    QTimer* m_readoutTimer = nullptr;
    QLabel* m_busy = nullptr;
};
