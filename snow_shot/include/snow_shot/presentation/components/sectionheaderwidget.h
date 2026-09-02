#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_SECTIONHEADERWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_SECTIONHEADERWIDGET_H

#include <QFrame>
#include <QString>

class QString;
class QWidget;
class QLabel;
class QEvent;
namespace adqt::widgets {
class AdButton;
class AdPopconfirm;
}
namespace snow_shot::presentation::styles {
struct ThemeAliasMetricToken;
struct ThemeColorScheme;
} // namespace snow_shot::presentation::styles

class SectionHeaderWidget : public QFrame {
    Q_OBJECT

  public:
    explicit SectionHeaderWidget(
        const QString& title, const snow_shot::presentation::styles::ThemeAliasMetricToken& metric,
        QWidget* parent = nullptr);
    void setTitle(const QString& title);
    void setResetVisible(bool visible);
    void setResetEnabled(bool enabled);
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);

  signals:
    void resetRequested();

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void retranslateUi();
    void updateResetConfirmationText();

    QString m_title;
    QLabel* m_titleLabel = nullptr;
    adqt::widgets::AdButton* m_resetButton = nullptr;
    adqt::widgets::AdPopconfirm* m_resetPopconfirm = nullptr;
    bool m_resetVisible = true;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_SECTIONHEADERWIDGET_H
