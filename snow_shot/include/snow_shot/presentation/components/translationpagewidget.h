#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_TRANSLATIONPAGEWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_TRANSLATIONPAGEWIDGET_H

#include "snow_shot/presentation/styles/themecolorscheme.h"

#include <QWidget>
#include <QTimer>

class QAction;
class QLabel;
class QGridLayout;
class QHideEvent;
class PageContainerWidget;
class SnowShotApiClient;
namespace adqt::widgets {
class AdSelect;
class AdTextEdit;
class AdButton;
class AdAlert;
class AdContextMenu;
class AdSpin;
} // namespace adqt::widgets
namespace snow_shot::presentation {
class TranslationPageController;
}

class TranslationPageWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit TranslationPageWidget(QWidget* parent = nullptr, SnowShotApiClient* client = nullptr,
                                   int debounceMilliseconds = 1500);
    ~TranslationPageWidget() override;
    void deactivate();
    void setSourceText(const QString& text);
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void retranslateUi();
    void copyResult(bool closeWindow);

  signals:
    void closeWindowRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void syncState();
    void scheduleResultUpdate();
    void flushResultUpdate();
    void syncResultActions();
    void updateLayout();
    void updateResultOverlays();
    void updateResult(const QString& text);
    void dismissPopups();
    bool ownsFocusWidget(const QWidget* widget) const;

    snow_shot::presentation::TranslationPageController* m_controller = nullptr;
    PageContainerWidget* m_container = nullptr;
    QWidget* m_form = nullptr;
    QWidget* m_fields[3]{};
    QLabel* m_labels[3]{};
    adqt::widgets::AdSelect* m_selects[3]{};
    QGridLayout* m_formLayout = nullptr;
    QGridLayout* m_editorsLayout = nullptr;
    adqt::widgets::AdTextEdit* m_source = nullptr;
    adqt::widgets::AdTextEdit* m_result = nullptr;
    adqt::widgets::AdSpin* m_resultSpin = nullptr;
    QWidget* m_resultPane = nullptr;
    adqt::widgets::AdButton* m_swap = nullptr;
    adqt::widgets::AdButton* m_resultCopy = nullptr;
    adqt::widgets::AdButton* m_floating = nullptr;
    QAction* m_copy = nullptr;
    QAction* m_copyClose = nullptr;
    adqt::widgets::AdButton* m_retry = nullptr;
    adqt::widgets::AdAlert* m_error = nullptr;
    adqt::widgets::AdContextMenu* m_menu = nullptr;
    QLabel* m_status = nullptr;
    QTimer m_resultUpdate;
    QString m_renderedResult;
    snow_shot::presentation::styles::ThemeColorScheme m_scheme;
    bool m_syncing = false;
    bool m_layoutQueued = false;
    bool m_active = true;
    int m_layoutColumns = 0;
    int m_formColumns = 0;
};

#endif
