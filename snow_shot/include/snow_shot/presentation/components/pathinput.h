#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_PATHINPUT_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_PATHINPUT_H

#include "icon_core.h"
#include "widgets/input_line_edit.h"

#include <QString>
#include <QWidget>

namespace adqt::widgets {
class AdButton;
class AdFieldGroup;
} // namespace adqt::widgets

class DirectoryPathInput : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(QString placeholderText READ placeholderText WRITE setPlaceholderText)
    Q_PROPERTY(bool allowClear READ allowClear WRITE setAllowClear)
    Q_PROPERTY(bool readOnly READ readOnly WRITE setReadOnly)
    Q_PROPERTY(adqt::widgets::AdLineEdit::ControlSize controlSize READ controlSize WRITE
                   setControlSize NOTIFY controlSizeChanged)
    Q_PROPERTY(adqt::widgets::AdLineEdit::Variant variant READ variant WRITE setVariant NOTIFY
                   variantChanged)
    Q_PROPERTY(
        adqt::widgets::AdLineEdit::Status status READ status WRITE setStatus NOTIFY statusChanged)

  public:
    explicit DirectoryPathInput(QWidget* parent = nullptr);
    ~DirectoryPathInput() override;

    [[nodiscard]] QString text() const;
    void setText(const QString& text);
    void clear();

    [[nodiscard]] QString placeholderText() const;
    void setPlaceholderText(const QString& text);

    [[nodiscard]] bool allowClear() const;
    void setAllowClear(bool allow);

    [[nodiscard]] bool readOnly() const;
    void setReadOnly(bool readOnly);

    [[nodiscard]] adqt::widgets::AdLineEdit::ControlSize controlSize() const;
    void setControlSize(adqt::widgets::AdLineEdit::ControlSize size);

    [[nodiscard]] adqt::widgets::AdLineEdit::Variant variant() const;
    void setVariant(adqt::widgets::AdLineEdit::Variant variant);
    [[nodiscard]] adqt::widgets::AdLineEdit::Status status() const;
    void setStatus(adqt::widgets::AdLineEdit::Status status);

    [[nodiscard]] QString browseButtonText() const;
    void setBrowseButtonText(const QString& text);

    [[nodiscard]] adqt::widgets::AdLineEdit* lineEdit() const;
    [[nodiscard]] adqt::widgets::AdButton* browseButton() const;
    [[nodiscard]] adqt::widgets::AdFieldGroup* fieldGroup() const;

  signals:
    void textChanged(const QString& text);
    void textEdited(const QString& text);
    void editingFinished();
    void cleared();
    void browseRequested(const QString& currentPath);
    void controlSizeChanged(adqt::widgets::AdLineEdit::ControlSize size);
    void variantChanged(adqt::widgets::AdLineEdit::Variant variant);
    void statusChanged(adqt::widgets::AdLineEdit::Status status);

  protected:
    DirectoryPathInput(const adqt::icons::IconRef& browseIcon, QWidget* parent);

  private:
    adqt::widgets::AdFieldGroup* m_group = nullptr;
    adqt::widgets::AdLineEdit* m_lineEdit = nullptr;
    adqt::widgets::AdButton* m_browseButton = nullptr;
    QString m_browseButtonText;
};

class FilePathInput final : public DirectoryPathInput {
    Q_OBJECT

  public:
    explicit FilePathInput(QWidget* parent = nullptr);
};

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_PATHINPUT_H
