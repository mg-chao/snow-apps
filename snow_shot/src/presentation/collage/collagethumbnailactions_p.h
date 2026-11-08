// SPDX-License-Identifier: GPL-3.0-only
#ifndef SNOW_SHOT_PRESENTATION_COLLAGETHUMBNAILACTIONS_P_H
#define SNOW_SHOT_PRESENTATION_COLLAGETHUMBNAILACTIONS_P_H

#include "snow_shot/presentation/components/overlaycontrolbutton.h"
#include "snow_shot/presentation/components/icons/snowshoticons.h"
#include "antd_icons.h"
#include <QEvent>
#include <QTimer>

namespace snow_shot::presentation::image_stitching_detail {
class ThumbnailActions final : public QObject {
  public:
    ThumbnailActions(QWidget* thumbnail, const char* replaceName, const char* removeName)
        : QObject(thumbnail), m_thumbnail(thumbnail) {
        using components::OverlayControlButton;
        replace = new OverlayControlButton(OverlayControlButton::Intent::Primary,
                                           icons::custom::outlined::ReplaceImage(), thumbnail);
        replace->setObjectName(QString::fromLatin1(replaceName));
        remove = new OverlayControlButton(OverlayControlButton::Intent::Destructive,
                                          adqt::icons::antd::outlined::IconDelete(), thumbnail);
        remove->setObjectName(QString::fromLatin1(removeName));
        for (auto* button : {replace, remove}) {
            button->setSizeClass(adqt::widgets::AdButton::SizeClass::Small);
            button->setFixedSize(28, 28);
            button->setIconSize(QSize(16, 16));
            button->setFocusPolicy(Qt::StrongFocus);
            button->installEventFilter(this);
        }
        thumbnail->setMouseTracking(true);
        thumbnail->installEventFilter(this);
        position();
        reveal();
    }
    void setEditingEnabled(bool enabled, bool replacementAllowed) {
        replace->setEnabled(enabled && replacementAllowed);
        remove->setEnabled(enabled);
    }
    void retranslate(const QString& replaceText, const QString& removeText) {
        replace->setToolTip(replaceText);
        replace->setAccessibleName(replaceText);
        remove->setToolTip(removeText);
        remove->setAccessibleName(removeText);
    }
    adqt::widgets::AdButton* replace = nullptr;
    adqt::widgets::AdButton* remove = nullptr;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == m_thumbnail) {
            if (event->type() == QEvent::Enter || event->type() == QEvent::Leave ||
                event->type() == QEvent::Hide) {
                m_hovered = event->type() == QEvent::Enter;
                reveal();
            } else if (event->type() == QEvent::Resize) {
                position();
            }
        }
        if (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)
            QTimer::singleShot(0, this, [this] { reveal(); });
        return QObject::eventFilter(watched, event);
    }

  private:
    void position() {
        remove->move(m_thumbnail->width() - remove->width() - 4,
                     m_thumbnail->height() - remove->height() - 4);
        replace->move(remove->x() - replace->width() - 4, remove->y());
        replace->raise();
        remove->raise();
    }
    void reveal() {
        const bool visible =
            m_hovered || m_thumbnail->hasFocus() || replace->hasFocus() || remove->hasFocus();
        replace->setVisible(visible);
        remove->setVisible(visible);
        m_thumbnail->update();
    }
    QWidget* m_thumbnail;
    bool m_hovered = false;
};
} // namespace snow_shot::presentation::image_stitching_detail
#endif
