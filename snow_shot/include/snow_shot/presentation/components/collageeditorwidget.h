// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_COLLAGEEDITORWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_COLLAGEEDITORWIDGET_H

#include "snow_shot/presentation/collagedocument.h"

#include <QWidget>
#include <QStringList>
#include <memory>

namespace snow_shot::presentation::components {
class ImageStitchingPdfPageGrid;
class ImageStitchingPdfTemplateWidget;

class ImageStitchingPreviewCanvas final : public QWidget {
    Q_OBJECT
  public:
    explicit ImageStitchingPreviewCanvas(ImageStitchingDocument* document,
                                         QWidget* parent = nullptr);
    ~ImageStitchingPreviewCanvas() override;
    [[nodiscard]] qreal zoom() const;
    [[nodiscard]] QPointF offset() const;
    [[nodiscard]] QRectF imageViewportRect(quint64 id) const;
    [[nodiscard]] QRectF cellViewportRect(quint64 id) const;
    void fitToCanvas();
    void centerImage(quint64 id);
    void preserveViewport();
    void setEditingEnabled(bool enabled);
    void setRenderingEnabled(bool enabled);
    void retranslateUi();

  protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void hideEvent(QHideEvent*) override;
    void showEvent(QShowEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;
    void changeEvent(QEvent*) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

class ImageStitchingEditorWidget final : public QWidget {
    Q_OBJECT
  public:
    explicit ImageStitchingEditorWidget(ImageStitchingDocument* document,
                                        QWidget* parent = nullptr);
    ~ImageStitchingEditorWidget() override;
    void setLoading(bool loading);
    void setExportBusy(bool busy);
    void retranslateUi();
    [[nodiscard]] ImageStitchingPreviewCanvas* canvas() const;
    [[nodiscard]] ImageStitchingPdfPageGrid* pdfGrid() const;
    [[nodiscard]] ImageStitchingPdfTemplateWidget* pdfTemplateWidget() const;
    [[nodiscard]] QWidget* outputDescriptionWidget() const;

  signals:
    void addImagesRequested();
    void imageFilesDropped(const QStringList& paths);
    void replaceImageRequested(quint64 id);
    void clearRequested();
    void settingsEdited(const QString& field);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void hideEvent(QHideEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation::components

#endif
