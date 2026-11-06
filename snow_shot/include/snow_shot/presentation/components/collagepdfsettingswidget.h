// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <QWidget>
#include <memory>

namespace snow_shot::presentation {
class ImageStitchingDocument;
}

namespace snow_shot::presentation::components {

// The text editors retain incomplete input independently of the last valid page layout.
class ImageStitchingPdfSettingsWidget final : public QWidget {
    Q_OBJECT
  public:
    explicit ImageStitchingPdfSettingsWidget(ImageStitchingDocument* document,
                                             QWidget* parent = nullptr);
    ~ImageStitchingPdfSettingsWidget() override;
    void retranslateUi();

  signals:
    void settingsEdited(const QString& field);

  protected:
    void changeEvent(QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::presentation::components
