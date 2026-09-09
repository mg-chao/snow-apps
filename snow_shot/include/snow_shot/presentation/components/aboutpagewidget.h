#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_ABOUTPAGEWIDGET_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_ABOUTPAGEWIDGET_H

#include <QUrl>
#include <QWidget>

#include <functional>
#include <memory>

namespace snow_shot::presentation::styles {
struct ThemeColorScheme;
}

class AboutPageWidget final : public QWidget {
    Q_OBJECT

  public:
    using UrlOpener = std::function<bool(const QUrl&)>;

    explicit AboutPageWidget(QWidget* parent = nullptr, UrlOpener urlOpener = {});
    ~AboutPageWidget() override;

  protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Ui;
    void applyTheme(const snow_shot::presentation::styles::ThemeColorScheme& scheme);
    void retranslateUi();
    void updateLayout();
    void openProjectLink(const QUrl& url);

    const QString m_version;
    const UrlOpener m_urlOpener;
    std::unique_ptr<Ui> m_ui;
    QUrl m_failedUrl;
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_ABOUTPAGEWIDGET_H
