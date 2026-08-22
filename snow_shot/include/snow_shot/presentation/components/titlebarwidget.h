#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_TITLEBARWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_TITLEBARWIDGET_H

#include <QColor>
#include <QFrame>
#include <QPixmap>

class QAbstractButton;
class QEvent;
class QPaintEvent;
class QWidget;
namespace snow_shot::presentation::styles {
struct ThemeAliasMetricToken;
struct ThemeColorScheme;
} // namespace snow_shot::presentation::styles

class TitleBarWidget : public QFrame {
    Q_OBJECT

  public:
    explicit TitleBarWidget(const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
                            QWidget* parent = nullptr);
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);

    QAbstractButton* minimizeButton() const;
    QAbstractButton* closeButton() const;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void retranslateUi();

    QAbstractButton* m_minimizeButton = nullptr;
    QAbstractButton* m_closeButton = nullptr;
    int m_logoHeight = 17;
    QColor m_logoColor = QColor(Qt::black);
    QPixmap m_logoPixmap;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_TITLEBARWIDGET_H
