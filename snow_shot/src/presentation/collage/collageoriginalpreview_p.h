// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGEORIGINALPREVIEW_P_H
#define SNOW_SHOT_PRESENTATION_COLLAGEORIGINALPREVIEW_P_H

#include "snow_shot/presentation/collagedocument.h"
#include "widgets/image.h"
#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>
#include <limits>

namespace snow_shot::presentation::image_stitching_detail {
class OriginalReply final : public adqt::widgets::AdImageReply {
  public:
    OriginalReply(const ImageStitchingItem* item, const char* context, QObject* parent)
        : AdImageReply(parent), m_image(item ? item->image : QImage{}),
          m_owner(item ? item->resourceOwner : nullptr), m_context(context) {
        QTimer::singleShot(0, this, [this] {
            if (!isFinished()) {
                if (m_image.isNull())
                    fail(QCoreApplication::translate(m_context,
                                                     "The original image is unavailable"));
                else
                    succeed(m_image);
            }
        });
    }
    void abort() override {
        if (!isFinished())
            fail(QCoreApplication::translate(m_context, "Image preview was cancelled"));
    }

  private:
    QImage m_image;
    std::shared_ptr<void> m_owner;
    const char* m_context;
};

class OriginalLoader final : public adqt::widgets::AdImageLoader {
  public:
    OriginalLoader(ImageStitchingDocument* document, const char* context, QObject* parent)
        : AdImageLoader(parent), m_document(document), m_context(context) {}
    adqt::widgets::AdImageReply* load(const QUrl& source, const adqt::widgets::AdImageLoadOptions&,
                                      QObject* parent) override {
        const int index = m_document ? m_document->indexOf(source.path().toULongLong()) : -1;
        return new OriginalReply(index < 0 ? nullptr : &m_document->items()[index], m_context,
                                 parent);
    }

  private:
    QPointer<ImageStitchingDocument> m_document;
    const char* m_context;
};

class OriginalPreview final : public QObject {
  public:
    OriginalPreview(ImageStitchingDocument* document, QWidget* owner, const char* name,
                    const char* context)
        : QObject(owner), m_document(document) {
        m_viewer = new adqt::widgets::AdImageViewer(owner);
        m_viewer->setObjectName(QString::fromLatin1(name));
        m_viewer->setOwnerWindow(owner);
        m_viewer->setImageLoader(new OriginalLoader(document, context, m_viewer));
        m_model = new adqt::widgets::AdImageListModel(m_viewer);
        m_viewer->setModel(m_model);
        owner->installEventFilter(this);
        connect(document, &ImageStitchingDocument::changed, this, [this] {
            if (!m_viewer->isVisible() || m_revision == m_document->contentRevision())
                return;
            quint64 viewedId = m_document->selectedId();
            const auto originals = m_model->items();
            if (m_viewer->currentRow() >= 0 && m_viewer->currentRow() < originals.size())
                viewedId = originals[m_viewer->currentRow()].source.path().toULongLong();
            close();
            open(m_document->indexOf(viewedId) >= 0 ? viewedId : m_document->selectedId());
        });
    }
    void open(quint64 id) {
        const int index = m_document->indexOf(id);
        if (index < 0)
            return;
        if (m_revision != m_document->contentRevision()) {
            m_revision = m_document->contentRevision();
            adqt::widgets::AdImageItems items;
            items.reserve(m_document->items().size());
            for (const auto& item : m_document->items()) {
                QUrl source;
                source.setScheme(QStringLiteral("collage-original"));
                source.setPath(QString::number(item.id));
                source.setQuery(QString::number(item.image.cacheKey()));
                items.push_back({source, QFileInfo(item.label).fileName()});
            }
            m_model->setItems(items);
        }
        m_viewer->openAt(index);
    }
    void close() {
        m_viewer->close();
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() == QEvent::Hide)
            close();
        return QObject::eventFilter(watched, event);
    }

  private:
    ImageStitchingDocument* m_document;
    adqt::widgets::AdImageViewer* m_viewer;
    adqt::widgets::AdImageListModel* m_model;
    quint64 m_revision = std::numeric_limits<quint64>::max();
};
} // namespace snow_shot::presentation::image_stitching_detail
#endif
