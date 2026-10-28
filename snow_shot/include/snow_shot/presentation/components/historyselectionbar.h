#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_HISTORYSELECTIONBAR_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_HISTORYSELECTIONBAR_H

#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "snow_shot/presentation/styles/mainwindowcomponenttoken.h"

#include <QPainter>
#include <QVariant>
#include <QWidget>

// Shared selection surface for the two image history pages.
class HistorySelectionBar final : public QWidget {
  public:
    using QWidget::QWidget;

    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme) {
        m_background = scheme.map.colorFillQuaternary;
        m_surface = scheme.map.colorBgLayout;
        m_surface.setAlpha(255);
        m_radius = scheme.metricAlias.borderRadiusLG;
        update();
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event)
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(Qt::NoPen);
        // Scrolling records must not bleed through an affixed action panel,
        // including when a skin makes the normal page surfaces translucent.
        if (parentWidget() && parentWidget()->property("pageSticky").toBool()) {
            painter.setBrush(m_surface);
            painter.drawRoundedRect(QRectF(rect()), m_radius, m_radius);
        }
        painter.setBrush(
            snow_shot::presentation::styles::mainWindowBackgroundColor(this, m_background));
        painter.drawRoundedRect(QRectF(rect()), m_radius, m_radius);
    }

  private:
    QColor m_background;
    QColor m_surface;
    qreal m_radius = 0.0;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_HISTORYSELECTIONBAR_H
