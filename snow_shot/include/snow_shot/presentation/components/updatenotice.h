#pragma once

#include <QMessageBox>
#include <QUrl>
#include <functional>

class QPushButton;

namespace snow_shot::presentation {
class UpdateNotice final : public QMessageBox {
  public:
    using LinkOpener = std::function<bool(const QUrl&)>;
    explicit UpdateNotice(QString version, QWidget* parent = nullptr, LinkOpener opener = {});

  protected:
    void changeEvent(QEvent* event) override;

  private:
    void retranslate();
    QString m_version;
    QPushButton* m_download = nullptr;
    QPushButton* m_later = nullptr;
};
} // namespace snow_shot::presentation
