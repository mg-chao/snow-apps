#ifndef SNOW_SHOT_PRESENTATION_CANVASSTATUSREADOUT_H
#define SNOW_SHOT_PRESENTATION_CANVASSTATUSREADOUT_H

#include <QEvent>
#include <QLabel>

// Shared by pinned zoom/opacity and the recording canvas. Lifetime/timeout belongs to the owner.
class CanvasStatusReadout final : public QLabel {
  public:
    explicit CanvasStatusReadout(QWidget* parent) : QLabel(parent) {
        setAttribute(Qt::WA_NativeWindow, false);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setFocusPolicy(Qt::NoFocus);
        setAlignment(Qt::AlignCenter);
        setTextFormat(Qt::PlainText);
        setStyleSheet(QStringLiteral("QLabel { color: white; background-color: rgba(0, 0, 0, 150); "
                                     "padding: 3px 6px; border-radius: 4px; }"));
        hide();
    }

    void setText(const QString& text) {
        m_fullText = text;
        setToolTip(text);
        setAccessibleName(text);
        layoutIn(m_anchor.isValid() ? m_anchor : parentWidget()->rect());
    }

    void layoutIn(const QRect& anchor) {
        m_anchor = anchor;
        const int insetX = qMin(8, qMax(0, anchor.width() / 2));
        const int insetY = qMin(8, qMax(0, anchor.height() / 2));
        const int availableWidth = qMax(0, anchor.width() - insetX * 2);
        QLabel::setText(
            fontMetrics().elidedText(m_fullText, Qt::ElideRight, qMax(0, availableWidth - 12)));
        const QSize desired = sizeHint().boundedTo(QSize(availableWidth, anchor.height()));
        resize(desired);
        move(anchor.left() + insetX, anchor.top() + qMax(0, anchor.height() - height() - insetY));
        raise();
    }

  private:
    QString m_fullText;
    QRect m_anchor;
};

#endif // SNOW_SHOT_PRESENTATION_CANVASSTATUSREADOUT_H
