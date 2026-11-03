// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_COLLAGEPDFTEMPLATEWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_COLLAGEPDFTEMPLATEWIDGET_H

#include "snow_shot/presentation/collagedocument.h"

#include <QWidget>
#include <memory>

namespace snow_shot::presentation::components {

class CollagePdfTemplateCanvas final : public QWidget {
    Q_OBJECT
  public:
    explicit CollagePdfTemplateCanvas(CollageDocument* document, QWidget* parent = nullptr);
    ~CollagePdfTemplateCanvas() override;
    [[nodiscard]] int pageIndex() const;
    void setPageIndex(int pageIndex);
    [[nodiscard]] QRectF pageViewportRect() const;
    [[nodiscard]] QRectF imageViewportRect(quint64 id) const;
    [[nodiscard]] QRectF cellViewportRect(quint64 id) const;
    void setLoading(bool loading);
    void setExportBusy(bool busy);
    void retranslateUi();

  signals:
    void pageChanged(int pageIndex);
    void addImagesRequested();
    void clearRequested();
    void replaceImageRequested(quint64 id);
    void previewOriginalRequested(quint64 id);

  protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void keyPressEvent(QKeyEvent*) override;
    void hideEvent(QHideEvent*) override;
    void showEvent(QShowEvent*) override;
    void changeEvent(QEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// A single composed page and a bounded, virtualized strip of all input images.
class CollagePdfTemplateWidget final : public QWidget {
    Q_OBJECT
  public:
    explicit CollagePdfTemplateWidget(CollageDocument* document, QWidget* parent = nullptr);
    ~CollagePdfTemplateWidget() override;
    [[nodiscard]] CollagePdfTemplateCanvas* canvas() const;
    void setLoading(bool loading);
    void setExportBusy(bool busy);
    void retranslateUi();

  signals:
    void addImagesRequested();
    void clearRequested();
    void replaceImageRequested(quint64 id);

  protected:
    bool eventFilter(QObject*, QEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    void changeEvent(QEvent*) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation::components

#endif
