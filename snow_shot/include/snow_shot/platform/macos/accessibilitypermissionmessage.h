#pragma once

#include <QMessageBox>
#include <QUrl>
#include <functional>

namespace snow_shot::platform::macos {
class AccessibilityPermissionMessage final : public QMessageBox {
  public:
    explicit AccessibilityPermissionMessage(std::function<bool(const QUrl&)> openUrl = {});
    void present(std::function<void()> retry = {});

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void updateText();
    QPushButton* m_settings;
    QPushButton* m_retry;
    std::function<void()> m_retryAction;
};
} // namespace snow_shot::platform::macos
