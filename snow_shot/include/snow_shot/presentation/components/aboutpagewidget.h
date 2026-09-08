#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_ABOUTPAGEWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_ABOUTPAGEWIDGET_H

#include <QWidget>

class QLabel;
class QFrame;
class QTimer;
class QBoxLayout;
class PageContainerWidget;
namespace adqt::widgets {
class AdButton;
class AdDescriptions;
} // namespace adqt::widgets
namespace snow_shot::presentation::styles {
struct ThemeColorScheme;
}

class AboutPageWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit AboutPageWidget(QWidget* parent = nullptr);

  protected:
    void changeEvent(QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void retranslateUi();

    const QString m_version;
    PageContainerWidget* m_container = nullptr;
    QLabel* m_logo = nullptr;
    QLabel* m_productName = nullptr;
    QLabel* m_description = nullptr;
    QFrame* m_versionPanel = nullptr;
    QBoxLayout* m_versionLayout = nullptr;
    QLabel* m_versionCaption = nullptr;
    QLabel* m_versionValue = nullptr;
    adqt::widgets::AdButton* m_copyButton = nullptr;
    QTimer* m_copyFeedbackTimer = nullptr;
    adqt::widgets::AdDescriptions* m_details = nullptr;
    QLabel* m_licenseNote = nullptr;
    QLabel* m_copyright = nullptr;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_ABOUTPAGEWIDGET_H
