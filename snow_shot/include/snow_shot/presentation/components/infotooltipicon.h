#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_INFOTOOLTIPICON_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_INFOTOOLTIPICON_H

#include <QColor>
#include <QLabel>
#include <QString>

namespace adqt::widgets {
class AdTooltip;
}

class InfoTooltipIcon final : public QLabel {
    Q_OBJECT

  public:
    explicit InfoTooltipIcon(int iconSize, QWidget* parent = nullptr);

    [[nodiscard]] QString tooltipText() const;
    void setTooltipText(const QString& text);

    void setIconColor(const QColor& color);

    [[nodiscard]] adqt::widgets::AdTooltip* tooltipHost() const;

  private:
    void syncIcon();

    adqt::widgets::AdTooltip* m_tooltip = nullptr;
    QColor m_iconColor;
    int m_iconSize = 14;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_INFOTOOLTIPICON_H
