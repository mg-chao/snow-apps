#ifndef SNOW_SHOT_PLATFORM_MACOS_CAPTUREFAILUREMESSAGE_H
#define SNOW_SHOT_PLATFORM_MACOS_CAPTUREFAILUREMESSAGE_H

#include <QMessageBox>
#include <QUrl>

#include <functional>

class QPushButton;

namespace snow_shot::platform::macos {

// Application-owned, reusable and independent of disposable screenshot overlay windows.
class CaptureFailureMessage final : public QMessageBox {
  public:
    explicit CaptureFailureMessage(std::function<bool(const QUrl&)> openUrl = {});
    void present(const QString& error);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void updateText();

    QString m_error;
    QPushButton* m_settingsButton = nullptr;
};

} // namespace snow_shot::platform::macos

#endif // SNOW_SHOT_PLATFORM_MACOS_CAPTUREFAILUREMESSAGE_H
