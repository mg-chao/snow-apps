#ifndef SNOW_SHOT_PRESENTATION_COMPONENTS_FORMFIELDS_H
#define SNOW_SHOT_PRESENTATION_COMPONENTS_FORMFIELDS_H

#include "snow_shot/presentation/components/pathinput.h"
#include "snow_shot/presentation/translatabletext.h"
#include "snow_shot/presentation/styles/themecolorscheme.h"
#include "widgets/color_picker.h"
#include "widgets/combo_box.h"
#include "widgets/form.h"
#include "widgets/input_number.h"
#include "widgets/input_password_edit.h"
#include "widgets/input_text_edit.h"
#include "widgets/multi_select.h"
#include "widgets/radio_button_group.h"
#include "widgets/select.h"
#include "widgets/slider.h"
#include "widgets/switch.h"

#include <QObject>
#include <QPointer>
#include <QVariant>

#include <functional>
#include <memory>

class QLabel;
class QGridLayout;

namespace snow_shot::presentation::components::form_fields {

enum class Presentation { SettingsRow, VerticalDialog };
// Explicit publishes drafts until the owner calls notifyCommitted().
enum class CommitPolicy { Immediate, OnFinish, Explicit };

struct Metadata {
    QString id;
    settings::TranslatableText label;
    settings::TranslatableText description;
    settings::TranslatableText placeholder;
    settings::TranslatableText suffix;
};

struct Options {
    Presentation presentation = Presentation::VerticalDialog;
    QWidget* parent = nullptr;
    adqt::widgets::AdForm* form = nullptr;
    CommitPolicy commitPolicy = CommitPolicy::Immediate;
    int controlWidth = 230;
    bool popupInModal = false;
    bool searchEnabled = true;
    // AdComboBox shares its native search/edit state; editable governs that state.
    bool editable = false;
    bool allowClear = false;
    bool readOnly = false;
    bool required = false;
    bool hasFeedback = false;
};

struct Choice {
    QVariant value;
    settings::TranslatableText label;
    QString text;
    bool enabled = true;
};

struct NumberOptions {
    double minimum = -1000000000.0;
    double maximum = 1000000000.0;
    double step = 1.0;
    int decimals = 0;
    bool wheelStepEnabled = false;
};

struct CustomBinding {
    QWidget* control = nullptr;
    QWidget* focusWidget = nullptr;
    std::function<QVariant()> readValue;
    std::function<void(const QVariant&)> writeValue;
    std::function<void()> retranslate;
};

// The view owns its controller and editor. Place viewWidget(), never the editor itself.
class FormField final : public QObject {
    Q_OBJECT

  public:
    FormField(const Metadata& metadata, const Options& options, CustomBinding binding,
              bool bridgeControlChanges = true);
    ~FormField() override;

    [[nodiscard]] QWidget* viewWidget() const;
    [[nodiscard]] QWidget* controlWidget() const;
    [[nodiscard]] QWidget* focusWidget() const;
    [[nodiscard]] adqt::widgets::AdFormItem* item() const;
    [[nodiscard]] QVariant value() const;
    [[nodiscard]] const Metadata& metadata() const;
    [[nodiscard]] bool isSynchronizing() const;

    void syncValue(const QVariant& value);
    void synchronize(const std::function<void()>& update);
    void setMetadata(const Metadata& metadata);
    void setDescriptionOverride(const QString& description);
    void setChoices(const QVector<Choice>& choices);
    void setFeedback(const QStringList& errors = {}, const QStringList& warnings = {},
                     bool busy = false);
    void setFieldEnabled(bool enabled);
    void setFieldVisible(bool visible);
    [[nodiscard]] QLabel* feedbackLabel() const;
    void reserveFeedbackHeight(int pixels);
    void retranslateUi();
    void applyTheme(const styles::ThemeColorScheme& scheme);

    // Custom editors call these from their typed edit/finish signals.
    void notifyEdited();
    void notifyCommitted();
    void setChoiceUpdater(std::function<void(const QVector<Choice>&)> updater);

  signals:
    void valueEdited(const QVariant& value);
    void valueCommitted(const QVariant& value);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

template <typename Editor> struct Handle {
    FormField* field = nullptr;
    Editor* editor = nullptr;

    [[nodiscard]] adqt::widgets::AdFormItem* item() const {
        return field->item();
    }
    [[nodiscard]] QWidget* viewWidget() const {
        return field->viewWidget();
    }
    [[nodiscard]] QWidget* focusWidget() const {
        return field->focusWidget();
    }
};

void configureForm(adqt::widgets::AdForm* form);
void configureTwoColumnGrid(QGridLayout* grid);

Handle<adqt::widgets::AdLineEdit> text(const Metadata& metadata, const Options& options = {});
Handle<adqt::widgets::AdPasswordEdit> password(const Metadata& metadata,
                                               const Options& options = {});
Handle<adqt::widgets::AdTextEdit> textArea(const Metadata& metadata, const Options& options = {});
Handle<adqt::widgets::AdSelect>
select(const Metadata& metadata, const QVector<Choice>& choices = {}, const Options& options = {});
Handle<adqt::widgets::AdComboBox> comboBox(const Metadata& metadata,
                                           const QVector<Choice>& choices = {},
                                           const Options& options = {});
Handle<adqt::widgets::AdMultiSelect> multiSelect(const Metadata& metadata,
                                                 const QVector<Choice>& choices = {},
                                                 const Options& options = {});
Handle<adqt::widgets::AdSwitch> switchField(const Metadata& metadata, const Options& options = {});
Handle<adqt::widgets::AdInputNumber> number(const Metadata& metadata,
                                            const NumberOptions& numberOptions = {},
                                            const Options& options = {});
Handle<adqt::widgets::AdSlider> slider(const Metadata& metadata,
                                       const NumberOptions& numberOptions = {},
                                       const Options& options = {});
Handle<adqt::widgets::AdColorPicker> color(const Metadata& metadata, const Options& options = {});
Handle<adqt::widgets::AdRadioButtonGroup>
radio(const Metadata& metadata, const QVector<Choice>& choices = {}, const Options& options = {});
Handle<FilePathInput> filePath(const Metadata& metadata, const Options& options = {});
Handle<DirectoryPathInput> directoryPath(const Metadata& metadata, const Options& options = {});
Handle<QWidget> custom(const Metadata& metadata, CustomBinding binding,
                       const Options& options = {});

} // namespace snow_shot::presentation::components::form_fields

#endif // SNOW_SHOT_PRESENTATION_COMPONENTS_FORMFIELDS_H
