// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_COLLAGEPDFPAGEGRID_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_COLLAGEPDFPAGEGRID_H

#include "snow_shot/presentation/collagedocument.h"

#include <QWidget>
#include <QStringList>
#include <memory>

namespace snow_shot::presentation::components {

// Shares the document's immutable sources and order with the static-image editor.
// Page cards are materialized only around the visible rows.
class ImageStitchingPdfPageGrid final : public QWidget {
    Q_OBJECT

  public:
    explicit ImageStitchingPdfPageGrid(ImageStitchingDocument* document, QWidget* parent = nullptr);
    ~ImageStitchingPdfPageGrid() override;
    void setLoading(bool loading);
    void setExportBusy(bool busy);
    void retranslateUi();

  signals:
    void addImagesRequested();
    void imageFilesDropped(const QStringList& paths);
    void replaceImageRequested(quint64 id);
    void clearRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation::components

#endif
